#pragma once

#include "sentinel/core/runtime/ToolExecutionGateway.h"

#include <functional>
#include <memory>
#include <atomic>

namespace sentinel::core {

class AgentExecutionScheduler final : public std::enable_shared_from_this<AgentExecutionScheduler> {
public:
    struct Budget {
        explicit Budget(int maxActive) : limit(qBound(1, maxActive, 8)) {}
        int limit = 1;
        std::atomic_int active{0};
    };
    using SharedBudget = std::shared_ptr<Budget>;
    using Output = std::function<void(int, const QString&, ProcessStream, const QByteArray&)>;
    using Started = std::function<void(int)>;
    using Completion = std::function<void(QList<ToolExecutionResult>)>;

    AgentExecutionScheduler(const ToolExecutionGateway& gateway, const IToolExecutor& executor,
                            int maxParallelTools, SharedBudget budget = {});
    void start(QList<ToolExecutionRequest> calls, QString sessionId, QStringList callIds,
               Started started, Output output, Completion completion);
    void cancel();
    void setPersistenceCheck(std::function<bool()> check) {
        persistenceCheck_ = std::move(check);
    }

private:
    bool canStart(int index) const;
    bool conflicts(int left, int right) const;
    void pump();
    void finish(int index, ToolExecutionResult result);

    const ToolExecutionGateway& gateway_;
    const IToolExecutor& executor_;
    int limit_ = 1;
    QList<ToolExecutionRequest> calls_;
    QString sessionId_;
    QStringList callIds_;
    QList<ToolExecutionResult> results_;
    QList<bool> started_;
    QList<bool> finished_;
    QList<IToolExecutor::Cancel> cancels_;
    Output output_;
    Started onStarted_;
    std::function<bool()> persistenceCheck_;
    Completion completion_;
    int active_ = 0;
    SharedBudget budget_;
    bool dispatching_ = false;
    bool cancelled_ = false;
    bool completed_ = false;
};

} // namespace sentinel::core
