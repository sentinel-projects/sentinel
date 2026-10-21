// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sentinel::core {
// Process-local, explicitly registered owners only. This is never a profile-wide barrier.
class StorageAdmission final {
    struct Shared;

public:
    enum class State { Running, Quiescing, Quiescent, Maintenance, Resuming, Unavailable };
    enum class Result {
        Ready,
        Busy,
        Cancelled,
        TimedOut,
        StaleGeneration,
        Unavailable,
        Unsupported
    };
    class Writer final {
    public:
        Writer() = default;
        ~Writer();
        Writer(Writer&&) noexcept;
        Writer& operator=(Writer&&) noexcept;
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        explicit operator bool() const {
            return bool(shared_);
        }

    private:
        friend class StorageAdmission;
        explicit Writer(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}
        void release();
        std::shared_ptr<Shared> shared_;
    };
    struct Snapshot {
        State state;
        std::uint64_t generation;
        unsigned writers;
    };
    enum class Scope { LeafStores, LiveApplication };
    explicit StorageAdmission(Scope scope = Scope::LeafStores);
    ~StorageAdmission();
    StorageAdmission(const StorageAdmission&) = delete;
    StorageAdmission& operator=(const StorageAdmission&) = delete;
    Snapshot snapshot() const;
    Writer admit(std::uint64_t generation);
    // Caller must not hold a writer, SQL transaction, callback mutex or application event loop.
    // Only explicitly enrolled local owners/lifetimes are covered; all-domain preparation is
    // unsupported.
    Result prepareLocal(std::chrono::milliseconds timeout);
    Result prepareApplication() const {
        return Result::Unsupported;
    }
    Result enterMaintenance();
    Result resume();
    void cancel();
    void shutdown();

private:
    std::shared_ptr<Shared> shared_;
    const Scope scope_;
};
} // namespace sentinel::core
