// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/runtime/TransportDiagnostics.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QUuid>
#include <cmath>

namespace sentinel::core {

// Two bounded attempts per classification. Never copy arbitrary provider strings,
// URLs, prompts, headers or output into the persisted diagnostic contract.
inline QJsonArray sanitizedClassificationDiagnostics(const QJsonArray& attempts) {
    QJsonArray result;
    int inspected = 0;
    for (const auto& value : attempts) {
        if (++inspected > 2)
            break;
        if (!value.isObject())
            continue;
        const auto input = value.toObject();
        QJsonObject record{{QStringLiteral("stage"), QStringLiteral("classification")}};
        for (const auto* key : {"trace_id", "request_id"}) {
            const QUuid id(input.value(QLatin1String(key)).toString());
            if (!id.isNull())
                record.insert(QLatin1String(key), id.toString(QUuid::WithoutBraces));
        }
        const auto started = QDateTime::fromString(
            input.value(QStringLiteral("started_at_utc")).toString(), Qt::ISODateWithMs);
        if (started.isValid())
            record.insert(QStringLiteral("started_at_utc"),
                          started.toUTC().toString(Qt::ISODateWithMs));
        for (const auto* key :
             {"attempt", "deadline_ms", "elapsed_ms", "http_status", "provider_attempts",
              "output_budget_tokens", "prompt_bytes", "request_bytes", "estimated_input_tokens",
              "prompt_tokens", "completion_tokens", "total_tokens", "reasoning_tokens"}) {
            const auto item = input.value(QLatin1String(key));
            const double number = item.toDouble(-1);
            if (item.isDouble() && std::isfinite(number) && number >= 0 &&
                number <= 1000000000000.0 && number == std::floor(number))
                record.insert(QLatin1String(key), item);
        }
        for (const auto* key : {"provider_invoked", "cancelled", "timed_out", "output_truncated",
                                "parent_expired", "parent_cancelled"}) {
            const auto item = input.value(QLatin1String(key));
            if (item.isBool())
                record.insert(QLatin1String(key), item);
        }
        const auto category = input.value(QStringLiteral("category")).toString();
        if (QStringList{"None", "AuthenticationRequired", "ModelNotFound", "ProviderUnavailable",
                        "ConnectionFailed", "Timeout", "RateLimited", "RequestRejected",
                        "CapabilityUnsupported", "MalformedResponse", "Cancelled",
                        "ProviderFailure", "Offline"}
                .contains(category))
            record.insert(QStringLiteral("category"), category);
        const auto lifecycle = input.value(QStringLiteral("lifecycle")).toString();
        if (QStringList{"Pending", "Running", "Completed", "Cancelled", "TimedOut", "RateLimited",
                        "Failed"}
                .contains(lifecycle))
            record.insert(QStringLiteral("lifecycle"), lifecycle);
        if (input.value(QStringLiteral("transport_attempts")).isArray())
            record.insert(QStringLiteral("transport_attempts"),
                          sanitizedTransportDiagnostics(
                              input.value(QStringLiteral("transport_attempts")).toArray()));
        record.insert(QStringLiteral("upstream_cancellation"),
                      input.value(QStringLiteral("provider_invoked")).isBool() &&
                              !input.value(QStringLiteral("provider_invoked")).toBool()
                          ? QStringLiteral("not_requested")
                          : QStringLiteral("unconfirmed"));
        result.append(record);
    }
    return result;
}

} // namespace sentinel::core
