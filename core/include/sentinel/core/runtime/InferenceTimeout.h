// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/runtime/OperationDeadline.h"
#include <QJsonObject>
#include <QString>
#include <memory>

namespace sentinel::core {

// Optional per-request policy, not a second settings store. Zero retains legacy
// behavior. An explicit total/parent deadline remains independent and authoritative.
struct InferenceTimeoutOptions {
    int initialResponseMs = 0;
    int usefulProgressIdleMs = 0;
    bool requested() const {
        return initialResponseMs != 0 || usefulProgressIdleMs != 0;
    }
    bool valid() const {
        return initialResponseMs >= 0 && usefulProgressIdleMs >= 0;
    }
};

enum class InferenceProgressSupport { Unknown, Unavailable, ParsedVisibleText };
enum class InferenceOperation { VisibleTextStreaming, StructuredGeneration, ModelStartup };
enum class InferenceLocation { Local, Remote };
enum class InferenceStopReason {
    None,
    Cancelled,
    TotalDeadline,
    InitialResponse,
    UsefulProgressIdle
};

struct EffectiveInferenceTimeout {
    InferenceTimeoutOptions requested;
    InferenceProgressSupport support = InferenceProgressSupport::Unknown;
    InferenceLocation location = InferenceLocation::Local;
    int explicitTotalMs = 0;
    InferenceOperation operation = InferenceOperation::VisibleTextStreaming;
    bool accepted(bool boundedParent = false) const {
        return requested.valid() &&
               (!requested.requested() ||
                (operation == InferenceOperation::VisibleTextStreaming &&
                 support == InferenceProgressSupport::ParsedVisibleText &&
                 (requested.usefulProgressIdleMs > 0 || explicitTotalMs > 0 || boundedParent)));
    }
};

// One dispatch lifetime, on the request's existing event-loop thread. Headers end
// the initial response wait but start an idle wait; they are never useful text.
// Only a fully parsed protocol text delta can call visibleText(). No payload is
// retained. Check expiry BEFORE accepting progress (the exact boundary loses).
class InferenceProgressWatch final {
public:
    explicit InferenceProgressWatch(EffectiveInferenceTimeout policy,
                                    std::shared_ptr<const OperationDeadline> parent = {},
                                    OperationDeadline::Clock clock = {})
        : policy_(policy), parent_(std::move(parent)),
          clock_(clock ? std::move(clock) : OperationDeadline::Clock([] {
              return std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          })),
          start_(clock_()), idleStart_(start_) {
        parentRemainingAtDispatchMs_ = parent_ && parent_->bounded() ? parent_->remainingMs() : -1;
    }

    bool needsPolling() const {
        return policy_.requested.requested();
    }
    InferenceStopReason check(bool cancelled = false) {
        if (cancelled)
            return reason_ = InferenceStopReason::Cancelled;
        if (reason_ != InferenceStopReason::None)
            return reason_;
        const auto now = clock_();
        if ((parent_ && parent_->expired()) ||
            (policy_.explicitTotalMs > 0 && now - start_ >= policy_.explicitTotalMs))
            return reason_ = InferenceStopReason::TotalDeadline;
        if (!responded_ && policy_.requested.initialResponseMs > 0 &&
            now - start_ >= policy_.requested.initialResponseMs)
            return reason_ = InferenceStopReason::InitialResponse;
        // With initial wait disabled, idle starts at dispatch. Otherwise it starts
        // at headers or first useful text, whichever is observed first.
        if ((responded_ || policy_.requested.initialResponseMs == 0) &&
            policy_.requested.usefulProgressIdleMs > 0 &&
            now - idleStart_ >= policy_.requested.usefulProgressIdleMs)
            return reason_ = InferenceStopReason::UsefulProgressIdle;
        return reason_;
    }
    void responseObserved() {
        if (check() != InferenceStopReason::None || responded_)
            return;
        responded_ = true;
        idleStart_ = clock_();
        firstResponseMs_ = qMax<qint64>(0, idleStart_ - start_);
    }
    bool visibleText(const QString& parsedText) {
        if (check() != InferenceStopReason::None ||
            policy_.support != InferenceProgressSupport::ParsedVisibleText ||
            parsedText.trimmed().isEmpty())
            return false;
        responseObserved();
        idleStart_ = clock_();
        if (firstProgressMs_ < 0)
            firstProgressMs_ = qMax<qint64>(0, idleStart_ - start_);
        ++progressEvents_;
        return true;
    }
    QJsonObject diagnostics() const {
        const char* reason = reason_ == InferenceStopReason::Cancelled         ? "cancelled"
                             : reason_ == InferenceStopReason::TotalDeadline   ? "total_deadline"
                             : reason_ == InferenceStopReason::InitialResponse ? "initial_response"
                             : reason_ == InferenceStopReason::UsefulProgressIdle
                                 ? "useful_progress_idle"
                                 : "none";
        return {{"initial_response_ms", policy_.requested.initialResponseMs},
                {"useful_progress_idle_ms", policy_.requested.usefulProgressIdleMs},
                {"explicit_total_ms", policy_.explicitTotalMs},
                {"parent_bounded", parent_ && parent_->bounded()},
                {"parent_remaining_at_dispatch_ms", parentRemainingAtDispatchMs_},
                {"policy_origin",
                 policy_.requested.requested() ? "explicit_request" : "legacy_compatible"},
                {"operation", "visible_text_streaming"},
                {"model_hardware_calibration", "unavailable_not_used"},
                {"location", policy_.location == InferenceLocation::Local ? "local" : "remote"},
                {"progress_support", policy_.support == InferenceProgressSupport::ParsedVisibleText
                                         ? "parsed_visible_text"
                                     : policy_.support == InferenceProgressSupport::Unknown
                                         ? "unknown"
                                         : "unavailable"},
                {"response_observed", responded_},
                {"first_response_ms", firstResponseMs_},
                {"first_progress_ms", firstProgressMs_},
                {"progress_events", progressEvents_},
                {"stop_reason", reason}};
    }

private:
    EffectiveInferenceTimeout policy_;
    std::shared_ptr<const OperationDeadline> parent_;
    OperationDeadline::Clock clock_;
    int parentRemainingAtDispatchMs_ = -1;
    qint64 start_;
    qint64 idleStart_;
    qint64 firstResponseMs_ = -1;
    qint64 firstProgressMs_ = -1;
    qint64 progressEvents_ = 0;
    bool responded_ = false;
    InferenceStopReason reason_ = InferenceStopReason::None;
};
} // namespace sentinel::core
