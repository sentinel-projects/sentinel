// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/runtime/InferenceTimeout.h"
#include <QJsonDocument>
#include <QVariantMap>
#include <cmath>
#include <limits>

namespace sentinel::core {
enum class InferenceTimeoutMode { Compatibility, Extended, Custom };
struct InferenceTimeoutProfile {
    InferenceTimeoutMode mode = InferenceTimeoutMode::Compatibility;
    InferenceTimeoutOptions progress;
    int totalMs = 0;
    QString error;
    bool valid() const {
        return error.isEmpty();
    }
    static InferenceTimeoutProfile parse(const QString& raw) {
        InferenceTimeoutProfile result;
        if (raw.isEmpty())
            return result; // Absent legacy key: no migration/write.
        if (raw.size() > 1024) {
            result.error = "settings.invalid-timeout-profile";
            return result;
        }
        const auto doc = QJsonDocument::fromJson(raw.toUtf8());
        const auto obj = doc.object();
        if (!doc.isObject() || obj.size() != 5 || obj.value("version").toDouble(-1) != 1 ||
            !obj.value("mode").isString()) {
            result.error = "settings.invalid-timeout-profile";
            return result;
        }
        const auto mode = obj.value("mode").toString();
        if (mode == "compatibility")
            result.mode = InferenceTimeoutMode::Compatibility;
        else if (mode == "extended")
            result.mode = InferenceTimeoutMode::Extended;
        else if (mode == "custom")
            result.mode = InferenceTimeoutMode::Custom;
        else {
            result.error = "settings.invalid-timeout-profile";
            return result;
        }
        auto number = [&](const char* key, int& output) {
            const auto v = obj.value(key);
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < 0 ||
                v.toDouble() > std::numeric_limits<int>::max() ||
                std::floor(v.toDouble()) != v.toDouble())
                return false;
            output = int(v.toDouble());
            return true;
        };
        if (!number("initialMs", result.progress.initialResponseMs) ||
            !number("idleMs", result.progress.usefulProgressIdleMs) ||
            !number("totalMs", result.totalMs) ||
            (result.mode == InferenceTimeoutMode::Compatibility &&
             (result.totalMs != 0 || result.progress.requested())) ||
            (result.mode != InferenceTimeoutMode::Compatibility && result.totalMs <= 0) ||
            (result.mode == InferenceTimeoutMode::Extended &&
             (result.progress.initialResponseMs <= 0 || result.progress.usefulProgressIdleMs <= 0)))
            result.error = "settings.invalid-timeout-profile";
        return result;
    }
    QString json() const {
        return QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{"version", 1},
                            {"mode", mode == InferenceTimeoutMode::Compatibility ? "compatibility"
                                     : mode == InferenceTimeoutMode::Extended    ? "extended"
                                                                                 : "custom"},
                            {"initialMs", progress.initialResponseMs},
                            {"idleMs", progress.usefulProgressIdleMs},
                            {"totalMs", totalMs}})
                .toJson(QJsonDocument::Compact));
    }
};

// One resolver for settings projection and request admission. Chat only: Agent
// classification/planning, process and approval lifetimes are different owners.
enum class InferenceExecutionClass { Unknown, LocalProvider, CloudProvider };
struct ResolvedChatTimeout {
    InferenceExecutionClass executionClass = InferenceExecutionClass::Unknown;
    InferenceTimeoutOptions progress;
    int totalMs = 120000; // Existing Chat request default, not a hardware estimate.
    QString error;
    bool supported = false;
    bool compatibility = true;
    QVariantMap snapshot() const {
        return {{"accepted", error.isEmpty()},
                {"code", error},
                {"supported", supported},
                {"compatibility", compatibility},
                {"initialMs", progress.initialResponseMs},
                {"idleMs", progress.usefulProgressIdleMs},
                {"totalMs", totalMs},
                {"scope", "chat-text-stream"},
                {"hardwareCalibration", "unavailable"},
                {"providerKind", executionClass == InferenceExecutionClass::LocalProvider ? "local"
                                 : executionClass == InferenceExecutionClass::CloudProvider
                                     ? "cloud"
                                     : "unknown"}};
    }
};
inline ResolvedChatTimeout
resolveChatTimeout(const InferenceTimeoutProfile& profile, bool supported, int explicitProviderMs,
                   const std::shared_ptr<const OperationDeadline>& parent = {},
                   bool cancelled = false) {
    ResolvedChatTimeout result;
    result.supported = supported;
    result.compatibility = profile.mode == InferenceTimeoutMode::Compatibility;
    result.error = profile.error;
    if (!result.compatibility && !supported)
        result.error = "settings.timeout-unsupported";
    if (!result.compatibility) {
        result.progress = profile.progress;
        result.totalMs = profile.totalMs;
    }
    // Compatibility preserves the historical Chat 120 s request, including its
    // existing provider-option precedence. Explicit profiles additionally respect
    // the configured provider ceiling; they never silently override it.
    if (!result.compatibility && explicitProviderMs > 0)
        result.totalMs = qMin(result.totalMs, explicitProviderMs);
    if (parent && parent->bounded()) {
        if (parent->expired())
            result.error = "inference.total-deadline";
        else
            result.totalMs = parent->childTimeoutMs(result.totalMs);
    }
    if (result.totalMs > 0 && !result.compatibility) {
        if (result.progress.initialResponseMs > 0)
            result.progress.initialResponseMs =
                qMin(result.progress.initialResponseMs, result.totalMs);
        if (result.progress.usefulProgressIdleMs > 0)
            result.progress.usefulProgressIdleMs =
                qMin(result.progress.usefulProgressIdleMs, result.totalMs);
    }
    if (cancelled)
        result.error = "inference.cancelled";
    return result;
}
} // namespace sentinel::core
