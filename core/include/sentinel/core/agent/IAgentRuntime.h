// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/agent/AgentEvent.h"
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/agent/ContextEngine.h"
#include "sentinel/core/agent/AgentLoopState.h"
#include "sentinel/core/agent/AgentPipelineResult.h"
#include "sentinel/core/runtime/ToolDescriptor.h"
#include "sentinel/core/runtime/ToolInvocationPlan.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

namespace sentinel::core {

enum class AgentStatus {
    Unavailable,
    Ready,
    Busy,
    Error,
};

inline QString agentStatusName(AgentStatus status) {
    switch (status) {
    case AgentStatus::Unavailable:
        return QStringLiteral("Unavailable");
    case AgentStatus::Ready:
        return QStringLiteral("Ready");
    case AgentStatus::Busy:
        return QStringLiteral("Busy");
    case AgentStatus::Error:
        return QStringLiteral("Error");
    }

    return QStringLiteral("Unavailable");
}

struct AgentCapabilityDescriptor {
    QString id;
    QString description;
    bool enabled = false;
};

struct AgentRequest {
    QString prompt;
    QString requestedToolId;
};

struct AgentResponse {
    bool success = false;
    QString message;
    AgentStatus status = AgentStatus::Unavailable;
};

struct AgentSessionOptions {
    QString responseProfileInstructions;
    QList<ChatImage> inputImages;
    AgentContextInput::WorkspaceContext workspaceContext;
    QString workspaceName;
    QString presetId;
    QString profileVersion;
    bool autonomousMode = false;
    int maxParallelTools = 3;
    QString runType = QStringLiteral("interactive");
    QStringList availableToolIds;
    bool restrictAvailableTools = false;
    std::function<void(const AgentStepRecord&)> onStep;
    std::function<void(const QString&)> onStatus;
    std::function<void(const AgentLoopState&)> onFinished;
};

using AgentEventCallback = std::function<void(const AgentEvent&)>;

enum class AgentRuntimeErrorCode {
    None,
    InvalidSession,
    Busy,
    InvalidState,
    ExecutionFailed,
    Cancelled,
    Stuck,
    ApprovalDenied,
    ProviderFailure,
    ToolFailure,
    CapabilityUnavailable,
    SecurityDenied,
    IterationLimit,
    CompletionIndeterminate,
    VerificationFailed,
    Blocked,
};

struct AgentRuntimeError {
    AgentRuntimeErrorCode code = AgentRuntimeErrorCode::None;
    QString message;
    bool retryable = false;
};

class IAgentRuntime {
public:
    virtual ~IAgentRuntime() = default;

    virtual QString name() const = 0;
    virtual AgentStatus status() const = 0;
    virtual QList<AgentCapabilityDescriptor> capabilities() const = 0;
    virtual QList<ToolDescriptor> availableTools() const = 0;
    virtual ToolInvocationPlan plan(const AgentRequest& request) const = 0;
    virtual AgentResponse execute(const AgentRequest& request) = 0;

    // Session execution is optional for metadata-only runtimes. The desktop
    // installs an executing runtime when a step planner is available.
    virtual QString createSession() {
        return {};
    }
    virtual AgentLoopState submit(const QString&, const QString&) {
        return {};
    }
    virtual AgentLoopState resume(const QString&, bool) {
        return {};
    }
    virtual bool approve(const QString&, bool) {
        return false;
    }
    virtual bool cancel(const QString&) {
        return false;
    }
    virtual AgentLoopState sessionState(const QString&) const {
        return {};
    }
    virtual AgentRuntimeError error(const QString&) const {
        return {};
    }
    virtual void configureSession(const QString&, AgentSessionOptions) {}
    virtual void setToolPermissionState(QString) {}
    virtual bool start(const QString&, const QString&) {
        return false;
    }
    virtual bool continueSession(const QString&, bool) {
        return false;
    }
    // A false result retains ownership: the composition root must stay alive.
    virtual bool tryShutdown(int) {
        return !supportsSessions();
    }
    virtual void shutdown() {}
    virtual bool supportsSessions() const {
        return false;
    }
    virtual QString subscribe(AgentEventCallback) {
        return {};
    }
    virtual void unsubscribe(const QString&) {}
    virtual QList<AgentEvent> eventHistory(const QString&) const {
        return {};
    }
};

} // namespace sentinel::core
