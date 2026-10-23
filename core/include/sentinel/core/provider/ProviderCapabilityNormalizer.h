// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/model/ModelRouting.h"
#include "sentinel/core/provider/ProviderRegistry.h"

#include <QJsonObject>
#include <QString>

#include <optional>

// One place that knows how each local provider protocol spells the same
// concept, so callers can reason about "did the provider stop early?" and
// "how big is the runtime actually?" without hard-coding Ollama, LM Studio or
// llama.cpp field names into agent logic. Nothing here selects, redirects or
// replaces the active ModelBinding; it only interprets and shapes requests for
// the provider the caller already bound.
namespace sentinel::core::provider {

// ---------------------------------------------------------------------------
// Response normalization
// ---------------------------------------------------------------------------

// Why generation stopped, normalized across protocols. `Length` always means
// the visible answer is truncated and must not be treated as complete.
enum class FinishReason {
    Unknown,
    Stop,
    Length,
    ToolCalls,
    ContentFilter,
    Other,
};

QString finishReasonName(FinishReason reason);

// One normalized view of a response's completion state and token usage.
struct NormalizedResponseOutcome {
    FinishReason finishReason = FinishReason::Unknown;
    // True when the provider stopped at its output limit. The reply is
    // incomplete even if it happens to contain parseable text.
    bool truncated = false;
    bool hasVisibleText = false;
    std::optional<int> promptTokens;
    std::optional<int> outputTokens;
    // Hidden reasoning charged against the completion budget.
    std::optional<int> reasoningTokens;
    // Provider-native counters, -1 when the provider did not report them.
    qint64 promptEvalCount = -1;
    qint64 evalCount = -1;
    // Provider's raw stop-reason string, length-capped for diagnostics.
    QString rawFinishReason;
};

// OpenAI-compatible / llama.cpp server / LM Studio chat completion body:
// choices[0].finish_reason plus usage.prompt_tokens/completion_tokens and
// completion_tokens_details.reasoning_tokens.
NormalizedResponseOutcome normalizeOpenAiOutcome(const QJsonObject& body);

// Ollama native /api/generate and /api/chat payloads: done_reason, eval_count,
// prompt_eval_count. A response with done_reason "length" is truncated exactly
// like an OpenAI finish_reason of "length".
NormalizedResponseOutcome normalizeOllamaOutcome(const QJsonObject& body);

// Writes the normalized completion state and usage into an allowlisted
// diagnostics object. Never invents usage numbers the provider omitted.
void applyOutcomeToDiagnostics(const NormalizedResponseOutcome& outcome, QJsonObject& diagnostics);

// Maps a normalized outcome onto a reply's allowlisted diagnostics and typed
// completion state.
void applyOutcomeToReply(const NormalizedResponseOutcome& outcome, ChatProviderReply& reply);

// ---------------------------------------------------------------------------
// Request shaping
// ---------------------------------------------------------------------------

// `ReasoningControl` (the caller's reasoning/thinking intent) is declared in
// IChatProvider.h because it travels on ChatRequestOptions. Providers map it to
// the field their own protocol documents; a protocol with no documented control
// emits nothing rather than borrowing another protocol's field name.
QString reasoningControlName(ReasoningControl control);

// Capability view derived from the immutable provider registry and the caller's
// existing ModelBinding. `loadedContextTokens` is the running server's capacity
// for the loaded model; `modelContextLimit` is the published maximum. Admission
// must use the first when present and must never report the second as if it
// were the first.
struct ProviderRequestProfile {
    QString providerId;
    QString modelId;
    ProviderProtocol protocol = ProviderProtocol::OpenAiCompatible;
    ProviderEndpoint endpoint = ProviderEndpoint::FixedLocal;
    ProviderKind kind = ProviderKind::Local;
    // The context the server actually configured for the loaded model.
    std::optional<int> loadedContextTokens;
    // The model's published maximum context. Upper bound only.
    std::optional<int> modelContextLimit;
    std::optional<int> maxOutputTokens;
    bool supportsReasoningControl = false;
    bool supportsJsonFormat = false;
    bool supportsNativeTools = false;
    bool supportsStructuredOutput = false;

    // The capacity to budget against: the loaded value when known, otherwise
    // the published maximum, otherwise nothing. `capacityConfidence` reports
    // which one it is so diagnostics never present a guess as measurement.
    std::optional<int> effectiveContextTokens() const;
    QString capacityConfidence() const;
};

ProviderRequestProfile requestProfile(const ModelBinding& binding);

// Ollama `/api/generate` and `/api/chat` "options" sub-object. `think` is
// Ollama's documented reasoning switch and `num_predict` is its documented
// output limit. `num_ctx` is only emitted when the caller already knows the
// server's loaded capacity, because it is a server-side load parameter and not
// a per-request output control.
QJsonObject ollamaOptionFields(ReasoningControl reasoning, int maxOutputTokens,
                               std::optional<int> loadedContextTokens);

// OpenAI-compatible body merge for LM Studio, llama.cpp server and
// openai-compatible-local. `chat_template_kwargs` is llama.cpp's documented
// template switch; `reasoning_effort` is LM Studio's. Hosted OpenAI,
// Anthropic and Gemini endpoints never receive either field.
QJsonObject openAiCompatibleReasoningFields(ReasoningControl reasoning, ProviderEndpoint endpoint);

// Convenience: the typed truncation diagnostic key written to
// ChatProviderReply::diagnostics whenever a provider stopped at its output
// limit. Consumers read the flag instead of matching error prose.
extern const QString kOutputLimitReachedDiagnostic;
extern const QString kFinishReasonDiagnostic;
extern const QString kReasoningTokensDiagnostic;

} // namespace sentinel::core::provider