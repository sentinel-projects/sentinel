// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <cmath>

namespace sentinel::core {

// One fixed-size frame per HTTP attempt, at most the existing three-send limit.
// Null means unobserved, not zero elapsed. Never accept transport-owned strings.
inline QJsonArray sanitizedTransportDiagnostics(const QJsonArray& input) {
    QJsonArray output;
    int inspected = 0;
    for (const auto& value : input) {
        if (++inspected > 3)
            break;
        if (!value.isObject())
            continue;
        const auto source = value.toObject();
        QJsonObject frame{{"version", 1}, {"connection_reuse", "unknown"}};
        const QUuid id(source.value("attempt_id").toString());
        if (!id.isNull())
            frame.insert("attempt_id", id.toString(QUuid::WithoutBraces));
        for (const auto* key :
             {"request_created_ms", "policy_admitted_ms", "reply_created_ms", "dns_started_ms",
              "tcp_connected_ms", "socket_connect_started_ms", "tls_encrypted_ms",
              "request_sent_ms", "metadata_received_ms", "first_body_ms", "qt_finished_ms",
              "error_ms", "deadline_expired_ms", "cancelled_ms", "observation_finished_ms",
              "http_status", "qt_network_error"}) {
            const auto item = source.value(QLatin1String(key));
            const double n = item.toDouble(-1);
            const double ceiling = qstrcmp(key, "http_status") == 0        ? 599
                                   : qstrcmp(key, "qt_network_error") == 0 ? 499
                                                                           : 1e12;
            const double floor = qstrcmp(key, "http_status") == 0 ? 100 : 0;
            frame.insert(QLatin1String(key), item.isDouble() && std::isfinite(n) && n >= floor &&
                                                     n <= ceiling && n == std::floor(n)
                                                 ? item
                                                 : QJsonValue(QJsonValue::Null));
        }
        for (const auto* key :
             {"policy_allowed", "response_completed", "partial_body", "metadata_signal_observed"}) {
            const auto item = source.value(QLatin1String(key));
            frame.insert(QLatin1String(key), item.isBool() ? item : QJsonValue(QJsonValue::Null));
        }
        output.append(frame);
    }
    return output;
}

} // namespace sentinel::core
