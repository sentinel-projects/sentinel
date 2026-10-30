// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/provider/ProviderCapabilityNormalizer.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sentinel::core::provider {

const QString kOutputLimitReachedDiagnostic = QStringLiteral("output_limit_reached");
const QString kFinishReasonDiagnostic = QStringLiteral("finish_reason");
const QString kReasoningTokensDiagnostic = QStringLiteral("reasoning_tokens");

namespace {

// Diagnostics are allowlisted and length-capped; raw provider strings never
// travel through unbounded.
QString boundedToken(QJsonValue value) {
    if (value.isString())
        return value.toString().left(64);
    return {};
}

std::optional<int> nonNegativeInt(const QJsonValue& value) {
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number != std::floor(number) ||
        number > static_cast<double>(std::numeric_limits<int>::max()))
        return std::nullopt;
    return static_cast<int>(number);
}

// finish_reason / done_reason spellings across OpenAI-compatible servers,
// llama.cpp and Ollama.
FinishReason classifyFinishReason(const QString& raw) {
    const auto reason = raw.trimmed().toLower();
    if (reason.isEmpty())
        return FinishReason::Unknown;
    if (reason == QLatin1String("stop") || reason == QLatin1String("end_of_turn") ||
        reason == QLatin1String("eos"))
        return FinishReason::Stop;
    if (reason == QLatin1String("length") || reason == QLatin1String("max_tokens"))
        return FinishReason::Length;
    if (reason == QLatin1String("tool_calls") || reason == QLatin1String("function_call"))
        return FinishReason::ToolCalls;
    if (reason == QLatin1String("content_filter"))
        return FinishReason::ContentFilter;
    return FinishReason::Other;
}

} // namespace

QString finishReasonName(FinishReason reason) {
    switch (reason) {
    case FinishReason::Stop:
        return QStringLiteral("stop");
    case FinishReason::Length:
        return QStringLiteral("length");
    case FinishReason::ToolCalls:
        return QStringLiteral("tool_calls");
    case FinishReason::ContentFilter:
        return QStringLiteral("content_filter");
    case FinishReason::Other:
        return QStringLiteral("other");
    case FinishReason::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

NormalizedResponseOutcome normalizeOpenAiOutcome(const QJsonObject& body) {
    NormalizedResponseOutcome outcome;
    const auto choices = body.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty()) {
        const auto choice = choices.first().toObject();
        outcome.rawFinishReason = boundedToken(choice.value(QStringLiteral("finish_reason")));
        outcome.finishReason = classifyFinishReason(outcome.rawFinishReason);
        const auto message = choice.value(QStringLiteral("message")).toObject();
        outcome.hasVisibleText = !message.value(QStringLiteral("content")).toString().isEmpty();
    }
    const auto usage = body.value(QStringLiteral("usage")).toObject();
    outcome.promptTokens = nonNegativeInt(usage.value(QStringLiteral("prompt_tokens")));
    outcome.outputTokens = nonNegativeInt(usage.value(QStringLiteral("completion_tokens")));
    outcome.reasoningTokens =
        nonNegativeInt(usage.value(QStringLiteral("completion_tokens_details"))
                           .toObject()
                           .value(QStringLiteral("reasoning_tokens")));
    // A provider that stops at the output limit truncates the answer whether or
    // not it says so in prose. Only an explicit stop reason clears this.
    outcome.truncated = outcome.finishReason == FinishReason::Length;
    return outcome;
}

NormalizedResponseOutcome normalizeOllamaOutcome(const QJsonObject& body) {
    NormalizedResponseOutcome outcome;
    outcome.rawFinishReason = boundedToken(body.value(QStringLiteral("done_reason")));
    outcome.finishReason = classifyFinishReason(outcome.rawFinishReason);
    // Ollama reports visible text and reasoning in the same stream. When the
    // provider exposes a reasoning block it is charged against the completion
    // budget and reported separately from the visible answer.
    outcome.hasVisibleText = !body.value(QStringLiteral("response")).toString().isEmpty();
    outcome.promptTokens = nonNegativeInt(body.value(QStringLiteral("prompt_eval_count")));
    outcome.outputTokens = nonNegativeInt(body.value(QStringLiteral("eval_count")));
    // Ollama has no separate reasoning counter; `thinking` text is part of the
    // reply and is not counted here. Consumers that ask for reasoning off never
    // have to subtract an unknown share.
    outcome.promptEvalCount = body.value(QStringLiteral("prompt_eval_count")).toDouble(-1);
    outcome.evalCount = body.value(QStringLiteral("eval_count")).toDouble(-1);
    // done_reason "length" is Ollama's truncation signal. A final chunk without
    // a done_reason is not evidence of completeness, so it stays Unknown.
    outcome.truncated = outcome.finishReason == FinishReason::Length;
    return outcome;
}

void applyOutcomeToDiagnostics(const NormalizedResponseOutcome& outcome, QJsonObject& diagnostics) {
    diagnostics.insert(kFinishReasonDiagnostic, finishReasonName(outcome.finishReason));
    diagnostics.insert(kOutputLimitReachedDiagnostic, outcome.truncated);
    if (outcome.promptTokens)
        diagnostics.insert(QStringLiteral("prompt_tokens"), *outcome.promptTokens);
    if (outcome.outputTokens)
        diagnostics.insert(QStringLiteral("completion_tokens"), *outcome.outputTokens);
    if (outcome.reasoningTokens)
        diagnostics.insert(kReasoningTokensDiagnostic, *outcome.reasoningTokens);
    if (outcome.evalCount >= 0)
        diagnostics.insert(QStringLiteral("eval_count"), static_cast<qint64>(outcome.evalCount));
    if (outcome.promptEvalCount >= 0)
        diagnostics.insert(QStringLiteral("prompt_eval_count"),
                           static_cast<qint64>(outcome.promptEvalCount));
}

void preserveReportedUsage(const QJsonObject& body, QJsonObject& diagnostics) {
    const auto copy = [&](const QJsonObject& source, const char* from, const char* to) {
        const auto value = source.value(QLatin1String(from));
        const double n = value.toDouble(-1);
        if (value.isDouble() && std::isfinite(n) && n >= 0 &&
            n <= std::numeric_limits<int>::max() && n == std::floor(n))
            diagnostics.insert(QLatin1String(to), int(n));
    };
    for (const auto& usage : {body.value("usage").toObject(),
                              body.value("message").toObject().value("usage").toObject()}) {
        copy(usage, "prompt_tokens", "prompt_tokens");
        copy(usage, "completion_tokens", "completion_tokens");
        copy(usage, "total_tokens", "total_tokens");
        copy(usage, "input_tokens", "prompt_tokens");
        copy(usage, "output_tokens", "completion_tokens");
        copy(usage.value("completion_tokens_details").toObject(), "reasoning_tokens",
             "reasoning_tokens");
    }
    const auto gemini = body.value("usageMetadata").toObject();
    copy(gemini, "promptTokenCount", "prompt_tokens");
    copy(gemini, "candidatesTokenCount", "completion_tokens");
    copy(gemini, "totalTokenCount", "total_tokens");
    copy(gemini, "thoughtsTokenCount", "reasoning_tokens");
    copy(body, "prompt_eval_count", "prompt_tokens");
    copy(body, "eval_count", "completion_tokens");
}

void applyOutcomeToReply(const NormalizedResponseOutcome& outcome, ChatProviderReply& reply) {
    applyOutcomeToDiagnostics(outcome, reply.diagnostics);
    reply.completion = outcome.truncated        ? ResponseCompletion::Truncated
                       : outcome.hasVisibleText ? ResponseCompletion::Complete
                                                : ResponseCompletion::Empty;
}

QString reasoningControlName(ReasoningControl control) {
    switch (control) {
    case ReasoningControl::Disabled:
        return QStringLiteral("disabled");
    case ReasoningControl::Low:
        return QStringLiteral("low");
    case ReasoningControl::Default:
        break;
    }
    return QStringLiteral("default");
}

std::optional<int> ProviderRequestProfile::effectiveContextTokens() const {
    if (loadedContextTokens && *loadedContextTokens > 0)
        return loadedContextTokens;
    if (modelContextLimit && *modelContextLimit > 0)
        return modelContextLimit;
    return std::nullopt;
}

QString ProviderRequestProfile::capacityConfidence() const {
    if (loadedContextTokens && *loadedContextTokens > 0)
        return QStringLiteral("server_configured");
    if (modelContextLimit && *modelContextLimit > 0)
        return QStringLiteral("model_maximum");
    return QStringLiteral("unknown");
}

ProviderRequestProfile requestProfile(const ModelBinding& binding) {
    ProviderRequestProfile profile;
    profile.providerId = binding.providerId;
    profile.modelId = binding.modelId;

    const auto* registration = ProviderRegistry::builtins().find(binding.providerId);
    if (registration) {
        profile.protocol = registration->protocol;
        profile.endpoint = registration->endpoint;
        profile.kind = registration->kind;
    }

    // The binding already carries the merged capability snapshot produced by the
    // model service. `contextCapacity` is what tells a server-configured value
    // apart from a catalog maximum; a binding that never learned the difference
    // reports both as the model maximum rather than claiming runtime knowledge.
    if (binding.capabilities.contextWindow && *binding.capabilities.contextWindow > 0) {
        if (binding.capabilities.contextCapacity == ContextCapacityKind::ServerConfigured)
            profile.loadedContextTokens = binding.capabilities.contextWindow;
        else
            profile.modelContextLimit = binding.capabilities.contextWindow;
    }
    if (binding.capabilities.maxOutputTokens && *binding.capabilities.maxOutputTokens > 0)
        profile.maxOutputTokens = binding.capabilities.maxOutputTokens;

    // Reasoning control is a property of the wire protocol, not of a model
    // card. Ollama documents `think`; llama.cpp documents `chat_template_kwargs`
    // and LM Studio documents `reasoning_effort`. Hosted OpenAI/Anthropic/Gemini
    // endpoints are left alone: their reasoning controls differ and must not be
    // guessed here.
    switch (profile.protocol) {
    case ProviderProtocol::Ollama:
        profile.supportsReasoningControl = true;
        profile.supportsJsonFormat = true;
        break;
    case ProviderProtocol::OpenAiCompatible:
        profile.supportsReasoningControl = profile.endpoint == ProviderEndpoint::LmStudio ||
                                           profile.endpoint == ProviderEndpoint::LlamaCpp ||
                                           profile.endpoint == ProviderEndpoint::FixedLocal;
        profile.supportsJsonFormat = true;
        break;
    default:
        break;
    }
    profile.supportsNativeTools =
        binding.capabilities.nativeToolCalling == CapabilitySupport::Supported;
    profile.supportsStructuredOutput =
        binding.capabilities.structuredOutput == CapabilitySupport::Supported;
    return profile;
}

QJsonObject ollamaOptionFields(ReasoningControl reasoning, int maxOutputTokens,
                               std::optional<int> loadedContextTokens) {
    QJsonObject options;
    if (maxOutputTokens > 0)
        options.insert(QStringLiteral("num_predict"), maxOutputTokens);
    // `think` is Ollama's documented reasoning switch. Only an explicit
    // Disabled/Low request writes it; Default leaves the model's own setting.
    switch (reasoning) {
    case ReasoningControl::Disabled:
        options.insert(QStringLiteral("think"), false);
        break;
    case ReasoningControl::Low:
        options.insert(QStringLiteral("think"), QStringLiteral("low"));
        break;
    case ReasoningControl::Default:
        break;
    }
    // num_ctx selects the server's context size at load time. Emitting it for a
    // capacity we actually know keeps the request inside the runtime we are
    // talking to; emitting a guessed value would relabel the model's theoretical
    // maximum as the server's real capacity.
    if (loadedContextTokens && *loadedContextTokens > 0)
        options.insert(QStringLiteral("num_ctx"), *loadedContextTokens);
    return options;
}

QJsonObject openAiCompatibleReasoningFields(ReasoningControl reasoning, ProviderEndpoint endpoint) {
    QJsonObject body;
    if (reasoning == ReasoningControl::Default)
        return body;
    switch (endpoint) {
    case ProviderEndpoint::LlamaCpp:
    case ProviderEndpoint::FixedLocal:
        // llama.cpp server forwards chat_template_kwargs to the model's chat
        // template, which is where the thinking switch lives.
        body.insert(QStringLiteral("chat_template_kwargs"),
                    QJsonObject{{QStringLiteral("enable_thinking"),
                                 reasoning != ReasoningControl::Disabled}});
        break;
    case ProviderEndpoint::LmStudio:
        // LM Studio's OpenAI-compatible server accepts reasoning_effort and
        // passes chat_template_kwargs through to its llama.cpp backend.
        body.insert(QStringLiteral("reasoning_effort"), reasoning == ReasoningControl::Disabled
                                                            ? QStringLiteral("none")
                                                            : QStringLiteral("low"));
        break;
    default:
        // Hosted endpoints and any endpoint whose reasoning control we do not
        // document receive nothing. Never send a field invented for another
        // protocol.
        break;
    }
    return body;
}

} // namespace sentinel::core::provider
