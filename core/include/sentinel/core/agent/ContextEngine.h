// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/agent/IAgentStepPlanner.h"
#include "sentinel/core/chat/ChatMessage.h"
#include "sentinel/core/interfaces/IMemoryStore.h"
#include "sentinel/core/skill/Skill.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace sentinel::core {
class IChatHistoryStore;

enum class AgentContextKind {
    Goal,
    Conversation,
    History,
    Workspace,
    Memory,
    Observation,
    Fact,
    Tool,
    Requirement,
    Skill,
    ResponseProfile
};
enum class AgentContextPriority { Critical, High, Normal, Low };

struct AgentContextItem {
    AgentContextKind kind;
    AgentContextPriority priority;
    QString source;
    QString content;
    bool untrusted = true;
};

struct AgentPlanningContext {
    QList<AgentContextItem> items;
    int estimatedTokens = 0;
    int omittedItems = 0;
    bool compacted = false;
};

struct AgentContextInput {
    struct WorkspaceContext {
        QString id;
        QString rootPath;
        QStringList includeHints;
        QStringList excludeHints;
        QString retrievalPreference;
        QString memoryScope;
    } workspaceContext;
    MemoryAccessContext memoryAccess;
    QString responseProfileInstructions;
    QString goal;
    QString workspace;
    const IChatHistoryStore* chatHistoryStore = nullptr;
    const IMemoryStore* memoryStore = nullptr;
    QList<AgentStepRecord> steps;
    QList<EvidenceRecord> evidence;
    QList<StructuredFact> facts;
    ObservationIntent intent;
    QList<ToolDescriptor> tools;
    QList<Skill> skills;
    int contextWindowTokens = 0;
    int maxOutputTokens = 0;
};

class ContextEngine final {
public:
    AgentPlanningContext build(const AgentContextInput& input) const;
};

} // namespace sentinel::core
