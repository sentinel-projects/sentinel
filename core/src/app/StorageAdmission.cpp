// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/StorageAdmission.h"
#include <utility>
namespace sentinel::core {
struct StorageAdmission::Shared {
    std::mutex mutex;
    std::condition_variable changed;
    State state = State::Running;
    std::uint64_t generation = 1;
    unsigned writers = 0;
    bool cancelled = false;
};
StorageAdmission::StorageAdmission(Scope scope)
    : shared_(std::make_shared<Shared>()), scope_(scope) {}
StorageAdmission::~StorageAdmission() {
    shutdown();
}
void StorageAdmission::Writer::release() {
    if (shared_) {
        {
            std::lock_guard lock(shared_->mutex);
            --shared_->writers;
        }
        shared_->changed.notify_all();
        shared_.reset();
    }
}
StorageAdmission::Writer::~Writer() {
    release();
}
StorageAdmission::Writer::Writer(Writer&& other) noexcept : shared_(std::move(other.shared_)) {}
StorageAdmission::Writer& StorageAdmission::Writer::operator=(Writer&& other) noexcept {
    if (this != &other) {
        release();
        shared_ = std::move(other.shared_);
    }
    return *this;
}
StorageAdmission::Snapshot StorageAdmission::snapshot() const {
    std::lock_guard lock(shared_->mutex);
    return {shared_->state, shared_->generation, shared_->writers};
}
StorageAdmission::Writer StorageAdmission::admit(std::uint64_t generation) {
    std::lock_guard lock(shared_->mutex);
    if (shared_->state != State::Running || generation != shared_->generation) {
        return {};
    }
    ++shared_->writers;
    return Writer(shared_);
}
StorageAdmission::Result StorageAdmission::prepareLocal(std::chrono::milliseconds timeout) {
    // Live request lifetimes and independent owners are not enrolled yet. Fail before
    // disturbing accepted callbacks or their durable evidence.
    if (scope_ == Scope::LiveApplication) {
        return Result::Unsupported;
    }
    if (timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(30)) {
        return Result::Unsupported;
    }
    std::unique_lock lock(shared_->mutex);
    if (shared_->state == State::Unavailable) {
        return Result::Unavailable;
    }
    if (shared_->state != State::Running) {
        return Result::Busy;
    }
    shared_->state = State::Quiescing;
    shared_->cancelled = false;
    const bool ready = shared_->changed.wait_for(lock, timeout, [&] {
        return shared_->writers == 0 || shared_->cancelled || shared_->state == State::Unavailable;
    });
    if (shared_->state == State::Unavailable) {
        return Result::Unavailable;
    }
    if (shared_->cancelled || !ready) {
        shared_->state = State::Running;
        return shared_->cancelled ? Result::Cancelled : Result::TimedOut;
    }
    shared_->state = State::Quiescent;
    return Result::Ready;
}
StorageAdmission::Result StorageAdmission::enterMaintenance() {
    std::lock_guard lock(shared_->mutex);
    if (shared_->state == State::Unavailable) {
        return Result::Unavailable;
    }
    if (shared_->state != State::Quiescent || shared_->writers) {
        return Result::Busy;
    }
    shared_->state = State::Maintenance;
    ++shared_->generation;
    return Result::Ready;
}
StorageAdmission::Result StorageAdmission::resume() {
    std::lock_guard lock(shared_->mutex);
    if (shared_->state == State::Unavailable) {
        return Result::Unavailable;
    }
    if (shared_->state != State::Quiescent && shared_->state != State::Maintenance) {
        return Result::Busy;
    }
    shared_->state = State::Resuming;
    shared_->state = State::Running;
    return Result::Ready;
}
void StorageAdmission::cancel() {
    {
        std::lock_guard lock(shared_->mutex);
        shared_->cancelled = true;
    }
    shared_->changed.notify_all();
}
void StorageAdmission::shutdown() {
    {
        std::lock_guard lock(shared_->mutex);
        shared_->state = State::Unavailable;
    }
    shared_->changed.notify_all();
}
} // namespace sentinel::core
