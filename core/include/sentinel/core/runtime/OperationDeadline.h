// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace sentinel::core {

// One monotonic operation lifetime. Zero preserves the existing unbounded-local
// contract. Copies of the shared owner never start a fresh child lifetime.
class OperationDeadline final {
public:
    using Clock = std::function<qint64()>;
    explicit OperationDeadline(int milliseconds, Clock clock = {})
        : limit_(qMax(0, milliseconds)), clock_(clock ? std::move(clock) : Clock([] {
              return std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          })),
          start_(clock_()) {}
    bool bounded() const {
        return limit_ > 0;
    }
    int remainingMs() const {
        return bounded() ? int(qMax<qint64>(0, limit_ - qMax<qint64>(0, clock_() - start_))) : 0;
    }
    bool expired() const {
        return bounded() && remainingMs() == 0;
    }
    // Callers must check expired before dispatch: zero is not an expired timeout
    // option, because existing transports interpret zero as unbounded.
    int childTimeoutMs(int requested) const {
        if (!bounded())
            return requested;
        const int remaining = remainingMs();
        return requested > 0 ? qMin(requested, remaining) : remaining;
    }

private:
    int limit_;
    Clock clock_;
    qint64 start_;
};

} // namespace sentinel::core
