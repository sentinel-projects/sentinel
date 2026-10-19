// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/context/ContextBudgetManager.h"
#include "sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
using namespace sentinel::core;
namespace {
ModelBinding model(int capacity = 8192) {
    ModelBinding binding;
    binding.providerId = "synthetic";
    binding.modelId = "fixed-model";
    binding.contextWindowTokens = capacity;
    binding.capabilities.contextWindow = capacity;
    binding.capabilities.provenance.contextWindow = ModelMetadataSource::RuntimeReported;
    return binding;
}
ChatRequestOptions continuation() {
    ChatRequestOptions options;
    options.maxOutputTokens = 1024;
    options.nativeToolCalling = true;
    options.completeToolResults = true;
    ToolDescriptor tool;
    tool.id = "read-file";
    tool.description = "Read a file";
    tool.inputSchema = {{"type", "object"}, {"properties", QJsonObject{}}};
    options.tools = {tool};
    options.priorToolCalls = {{"call-1", "read-file", {{"path", "example"}}, {}}};
    options.toolResults = {{"call-1", "complete observation"}};
    return options;
}
} // namespace
class ContextBudgetManagerTest final : public QObject {
    Q_OBJECT
private slots:
    void shortRequestAndBindingAreUnchanged() {
        const auto binding = model();
        ContextBudgetIdentity identity{"session", "workspace"};
        const auto result = ContextBudgetManager{}.assemble(
            binding, {}, {{"User: hello", ContextBudgetKind::Conversation, 0, true}}, {}, identity);
        identity.workspaceId = "other";
        QCOMPARE(result.identity.workspaceId, QString("workspace"));
        QVERIFY(result.accepted());
        QCOMPARE(result.prompt, QString("User: hello"));
        QCOMPARE(result.binding.providerId, binding.providerId);
        QCOMPARE(result.binding.modelId, binding.modelId);
        QCOMPARE(result.reportedCapacity.value(), 8192);
        QCOMPARE(result.reservedOutput, 2048);
        QVERIFY(*result.estimatedInput + result.reservedOutput <= result.effectiveCapacity);
        QVERIFY(result.diagnostics().value("actual_input_tokens").isNull());
    }
    void wholeTurnsUsePriorityAndOriginalOrder() {
        QList<ContextBudgetItem> items{
            {QString(9000, 'a'), ContextBudgetKind::Conversation, 1},
            {"recent first", ContextBudgetKind::Conversation, 10},
            {"recent second", ContextBudgetKind::Conversation, 20},
            {"active full request", ContextBudgetKind::Conversation, 0, true}};
        auto result = ContextBudgetManager{}.assemble(model(4096), {}, items);
        QCOMPARE(result.status, ContextBudgetStatus::Reduced);
        QCOMPARE(result.prompt, QString("recent first\n\nrecent second\n\nactive full request"));
        QCOMPARE(result.decisions.first().reason, ContextOmissionReason::Capacity);
        auto repeat = ContextBudgetManager{}.assemble(model(4096), {}, items);
        QCOMPARE(result.prompt, repeat.prompt);
    }
    void requiredInstructionsAndReserveRefuseRatherThanClip() {
        auto result = ContextBudgetManager{}.assemble(
            model(2100), {},
            {{"security and active request", ContextBudgetKind::Instruction, 0, true}});
        QCOMPARE(result.status, ContextBudgetStatus::InsufficientCapacity);
        QVERIFY(result.prompt.isEmpty());
        ChatRequestOptions options;
        options.maxOutputTokens = 128;
        result = ContextBudgetManager{}.assemble(
            model(2100), options,
            {{"security and active request", ContextBudgetKind::Instruction, 0, true}});
        QVERIFY(result.accepted());
        QCOMPARE(result.reservedOutput, 128);
    }
    void unknownAndMalformedCapacity() {
        ModelBinding unknown;
        auto result = ContextBudgetManager{}.assemble(unknown, {}, {{"hello"}});
        QVERIFY(result.accepted());
        QCOMPARE(result.effectiveCapacity, 8192);
        QVERIFY(!result.reportedCapacity);
        QVERIFY(result.diagnostics().value("reported_capacity").isNull());
        auto binding = model(65536);
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::Unknown;
        QCOMPARE(ContextBudgetManager{}.assemble(binding, {}, {{"hello"}}).effectiveCapacity, 8192);
        binding.capabilities.provenance.contextWindow = ModelMetadataSource::ProviderDefault;
        QCOMPARE(ContextBudgetManager{}.assemble(binding, {}, {{"hello"}}).effectiveCapacity, 8192);
        binding.capabilities.contextWindow = -1;
        QCOMPARE(ContextBudgetManager{}.assemble(binding, {}, {{"hello"}}).status,
                 ContextBudgetStatus::InvalidMetadata);
        binding = model();
        binding.capabilities.maxOutputTokens = 0;
        QCOMPARE(ContextBudgetManager{}.assemble(binding, {}, {{"hello"}}).status,
                 ContextBudgetStatus::InvalidMetadata);
    }
    void scopesProvenanceAndLargeRecords() {
        MemoryAccessContext own{"session", "workspace", {"workspace"}, false};
        ScopedMemoryRecord record;
        record.id = "stable-record";
        record.scope = MemoryScope::Session;
        record.ownerId = own.sessionId;
        record.workspaceId = own.workspaceId;
        record.key = "key";
        record.value = "orchid";
        record.provenance = MemoryProvenance::GeneratedSummary;
        record.sourceReference = "summary:source";
        record.createdAtUtc = record.updatedAtUtc = QDateTime::currentDateTimeUtc();
        auto included = ContextBudgetManager::memoryItem(record, own);
        auto foreign = record;
        foreign.scope = MemoryScope::Project;
        foreign.ownerId = foreign.workspaceId = "foreign";
        auto personal = record;
        personal.scope = MemoryScope::Personal;
        personal.ownerId.clear();
        personal.workspaceId.clear();
        auto large = record;
        large.id = "large";
        large.value = QString(65000, 'x');
        auto malformed = record;
        malformed.id.clear();
        auto result = ContextBudgetManager{}.assemble(
            model(), {},
            {included, ContextBudgetManager::memoryItem(foreign, own),
             ContextBudgetManager::memoryItem(personal, own),
             ContextBudgetManager::memoryItem(large, own),
             ContextBudgetManager::memoryItem(malformed, own)});
        QCOMPARE(result.status, ContextBudgetStatus::Reduced);
        QVERIFY(result.prompt.contains("stable-record"));
        QVERIFY(result.prompt.contains("generated-summary"));
        QVERIFY(result.prompt.contains("summary:source"));
        QVERIFY(result.prompt.contains("untrusted"));
        QVERIFY(!result.prompt.contains("foreign"));
        QCOMPARE(result.decisions.at(1).reason, ContextOmissionReason::Unauthorized);
        QCOMPARE(result.decisions.at(2).reason, ContextOmissionReason::Unauthorized);
        QCOMPARE(result.decisions.at(3).reason, ContextOmissionReason::Capacity);
        QCOMPARE(result.decisions.at(4).reason, ContextOmissionReason::InvalidMetadata);
        own.includePersonal = true;
        QVERIFY(ContextBudgetManager{}
                    .assemble(model(), {}, {ContextBudgetManager::memoryItem(personal, own)})
                    .prompt.contains("personal"));
        const auto diagnostics = QJsonDocument(result.diagnostics()).toJson();
        QVERIFY(!diagnostics.contains("orchid"));
        QVERIFY(!diagnostics.contains("stable-record"));
        QVERIFY(!diagnostics.contains("workspace"));
    }
    void schemasAndObservationsAreChargedAndNeverSplit() {
        auto options = continuation();
        const auto result = ContextBudgetManager{}.assemble(
            model(), options, {{"active task", ContextBudgetKind::Instruction, 0, true}});
        QVERIFY(result.accepted());
        const auto serialized =
            provider::buildOpenAiCompatibleNativeRequest("fixed-model", result.prompt, options);
        QVERIFY(serialized.error.isEmpty());
        const auto messages = serialized.body.value("messages").toArray();
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages.at(1)
                     .toObject()
                     .value("tool_calls")
                     .toArray()
                     .first()
                     .toObject()
                     .value("id")
                     .toString(),
                 QString("call-1"));
        QCOMPARE(messages.last().toObject().value("content").toString(),
                 QString("complete observation"));
        const auto bytes = QJsonDocument(serialized.body).toJson(QJsonDocument::Compact).size();
        QVERIFY(*result.estimatedInput >= (bytes + 2) / 3);
        options.toolResults[0].content = QString(9000, 'x') + "END_EVIDENCE";
        const auto largeAccepted =
            ContextBudgetManager{}.assemble(model(65536), options, {{"task"}});
        QVERIFY(largeAccepted.accepted());
        const auto complete = provider::buildOpenAiCompatibleNativeRequest(
            "fixed-model", largeAccepted.prompt, options);
        QCOMPARE(
            complete.body.value("messages").toArray().last().toObject().value("content").toString(),
            options.toolResults.first().content);
        options.toolResults[0].content = QString(40000, 'x');
        QCOMPARE(ContextBudgetManager{}.assemble(model(), options, {{"task"}}).status,
                 ContextBudgetStatus::InsufficientCapacity);
        options = continuation();
        options.tools[0].inputSchema.insert("description", QString(40000, 's'));
        QCOMPARE(ContextBudgetManager{}.assemble(model(), options, {{"task"}}).status,
                 ContextBudgetStatus::InsufficientCapacity);
        options.nativeToolCalling = false;
        options.priorToolCalls.clear();
        options.toolResults.clear();
        QVERIFY(ContextBudgetManager{}.assemble(model(), options, {{"task"}}).accepted());
    }
    void invalidNativeContinuationsRefuse() {
        auto options = continuation();
        options.toolResults.clear();
        QCOMPARE(ContextBudgetManager{}.assemble(model(), options, {{"task"}}).status,
                 ContextBudgetStatus::InvalidContinuation);
        options = continuation();
        options.toolResults[0].callId = "wrong";
        QCOMPARE(ContextBudgetManager{}.assemble(model(), options, {{"task"}}).status,
                 ContextBudgetStatus::InvalidContinuation);
        options = continuation();
        options.priorToolCalls[0].providerContinuation = "malformed";
        QCOMPARE(ContextBudgetManager{}.assemble(model(), options, {{"task"}}).status,
                 ContextBudgetStatus::InvalidContinuation);
    }
    void unicodeAndMultimodalUncertainty() {
        const QString text = QString::fromUtf8("İstanbul 🌸 漢字 é");
        auto result = ContextBudgetManager{}.assemble(
            model(), {},
            {{text, ContextBudgetKind::Conversation, 0, true}, {QString(65000, QChar(0x0131))}});
        QVERIFY(result.accepted());
        QCOMPARE(result.prompt, text);
        QVERIFY(!result.prompt.contains(QChar(0xfffd)));
        QVERIFY(result.uncertaintyAllowance.value() > 0);
        ChatRequestOptions images;
        images.images = {{"image/png", QByteArray("synthetic")}};
        result = ContextBudgetManager{}.assemble(model(), images, {{text}});
        QCOMPARE(result.status, ContextBudgetStatus::MultimodalUncertain);
        QVERIFY(!result.accepted());
    }
};
QTEST_GUILESS_MAIN(ContextBudgetManagerTest)
#include "test_context_budget_manager.moc"
