#include "sentinel/core/agent/AgentExecutionScheduler.h"

#include "sentinel/core/security/PathGuard.h"

#include <algorithm>
#include <utility>

namespace sentinel::core {
namespace {

bool mutableAccess(AccessMode access) {
    return access == AccessMode::Write || access == AccessMode::Delete ||
           access == AccessMode::Execute || access == AccessMode::Control;
}

bool overlapping(const AuthorizationRequest& a, const AuthorizationRequest& b) {
    if (a.domain != b.domain) return false;
    if (a.domain == SecurityDomain::FileSystem) {
        if (a.resource.isEmpty() || b.resource.isEmpty()) return true;
        return PathGuard::contains(a.resource, b.resource) ||
               PathGuard::contains(b.resource, a.resource);
    }
    if (a.resource.isEmpty() || b.resource.isEmpty()) return true;
    return a.resource == b.resource;
}

const PlannedToolInvocation& invocation(const ToolExecutionRequest& request) {
    return request.plan.invocations.first();
}

} // namespace

AgentExecutionScheduler::AgentExecutionScheduler(const ToolExecutionGateway& gateway,
                                                 const IToolExecutor& executor,
                                                 int maxParallelTools, SharedBudget budget)
    : gateway_(gateway), executor_(executor), limit_(qBound(1, maxParallelTools, 4)),
      budget_(std::move(budget)) {}

void AgentExecutionScheduler::start(QList<ToolExecutionRequest> calls, QString sessionId,
                                    QStringList callIds, Started started, Output output,
                                    Completion completion) {
    calls_ = std::move(calls);
    sessionId_ = std::move(sessionId);
    callIds_ = std::move(callIds);
    output_ = std::move(output);
    onStarted_ = std::move(started);
    completion_ = std::move(completion);
    results_.resize(calls_.size());
    started_.fill(false, calls_.size());
    finished_.fill(false, calls_.size());
    cancels_.resize(calls_.size());
    pump();
}

bool AgentExecutionScheduler::conflicts(int left, int right) const {
    const auto& a = invocation(calls_.at(left));
    const auto& b = invocation(calls_.at(right));
    if (!a.descriptorSnapshot || !b.descriptorSnapshot ||
        !a.descriptorSnapshot->parallelSafe || !b.descriptorSnapshot->parallelSafe ||
        !a.resourceSnapshot || !b.resourceSnapshot ||
        !a.resourceSnapshot->authorized || !b.resourceSnapshot->authorized)
        return true;
    for (const auto& first : a.resourceSnapshot->requests)
        for (const auto& second : b.resourceSnapshot->requests)
            if (overlapping(first, second) &&
                (mutableAccess(first.access) || mutableAccess(second.access)))
                return true;
    return false;
}

bool AgentExecutionScheduler::canStart(int index) const {
    const auto& action = invocation(calls_.at(index));
    for (const int dependency : action.dependsOn)
        if (dependency < 0 || dependency >= index || !finished_.at(dependency))
            return false;
    for (int earlier = 0; earlier < index; ++earlier)
        if (!finished_.at(earlier) && conflicts(earlier, index))
            return false;
    return true;
}

void AgentExecutionScheduler::pump() {
    if (dispatching_ || completed_) return;
    const auto keepAlive = shared_from_this();
    dispatching_ = true;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < calls_.size(); ++i) {
            if (started_.at(i)) continue;
            for (const int dependency : invocation(calls_.at(i)).dependsOn) {
                const bool invalid = dependency < 0 || dependency >= i;
                const bool failed = !invalid && finished_.at(dependency) &&
                    results_.at(dependency).status != ToolExecutionStatus::Succeeded &&
                    results_.at(dependency).status != ToolExecutionStatus::PlaceholderSucceeded;
                if (!invalid && !failed) continue;
                started_[i] = true;
                finished_[i] = true;
                results_[i].status = ToolExecutionStatus::Blocked;
                results_[i].summary = invalid
                    ? QStringLiteral("Invalid tool dependency.")
                    : QStringLiteral("Required tool dependency did not succeed.");
                changed = true;
                break;
            }
        }
    }
    while (!cancelled_ && active_ < limit_) {
        int next = -1;
        for (int i = 0; i < calls_.size(); ++i)
            if (!started_.at(i) && canStart(i)) { next = i; break; }
        if (next < 0) break;
        if (budget_) {
            int observed = budget_->active.load();
            bool acquired = false;
            while (observed < budget_->limit) {
                if (budget_->active.compare_exchange_weak(observed, observed + 1)) {
                    acquired = true;
                    break;
                }
            }
            if (!acquired) break;
        }
        started_[next] = true;
        ++active_;
        if (onStarted_) onStarted_(next);
        if (persistenceCheck_ && !persistenceCheck_()) {
            finish(next,
                   {ToolExecutionStatus::Blocked,
                    QStringLiteral("Agent audit persistence failed; tool dispatch refused.")});
            continue;
        }
        std::weak_ptr<AgentExecutionScheduler> weak = weak_from_this();
        auto cancel = gateway_.executeAsync(
            calls_.at(next), executor_, sessionId_, callIds_.value(next),
            [weak, next](const QString& processId, ProcessStream stream, const QByteArray& bytes) {
                if (auto state = weak.lock(); state && !state->cancelled_ && state->output_)
                    state->output_(next, processId, stream, bytes);
            },
            [weak, next](ToolExecutionResult result) {
                if (auto state = weak.lock()) state->finish(next, std::move(result));
            });
        if (!finished_.at(next)) cancels_[next] = std::move(cancel);
    }
    dispatching_ = false;
    if (std::all_of(finished_.cbegin(), finished_.cend(), [](bool done) { return done; }) &&
        !completed_) {
        completed_ = true;
        if (completion_) completion_(results_);
    }
}

void AgentExecutionScheduler::finish(int index, ToolExecutionResult result) {
    if (completed_ || finished_.at(index)) return;
    finished_[index] = true;
    cancels_[index] = {};
    --active_;
    if (budget_) budget_->active.fetch_sub(1);
    results_[index] = std::move(result);
    pump();
}

void AgentExecutionScheduler::cancel() {
    if (cancelled_ || completed_) return;
    cancelled_ = true;
    for (auto& request : calls_)
        for (auto& call : request.plan.invocations)
            if (call.cancellation) call.cancellation->store(true);
    for (auto& cancel : cancels_)
        if (cancel) cancel();
    if (budget_) {
        int startedNotFinished = 0;
        for (int i = 0; i < started_.size(); ++i)
            if (started_.at(i) && !finished_.at(i))
                ++startedNotFinished;
        if (startedNotFinished > 0)
            budget_->active.fetch_sub(startedNotFinished);
    }
    output_ = {};
    onStarted_ = {};
    completion_ = {};
    completed_ = true;
}

} // namespace sentinel::core
