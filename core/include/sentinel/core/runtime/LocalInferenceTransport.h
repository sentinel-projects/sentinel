// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/runtime/OperationDeadline.h"
#include <QNetworkAccessManager>

#include <memory>

namespace sentinel::core {

// Safely constrained transport seam beneath the native provider request builders.
//
// Production never sets this: `LMStudioConfig::networkFactory` and
// ModelService::setProviderNetworkFactoryForTesting stay empty, so the default
// factory creates a real QNetworkAccessManager and the existing
// NetworkPolicyService gate, authenticated HTTPS endpoints, host validation and
// TLS verification apply unchanged. A deterministic fixture may install an
// in-memory factory to observe the exact intended URL, headers and serialized
// request body while issuing no network request. This introduces no second
// provider registry, router or HTTP stack; the default implementation is the
// existing Qt transport.
class ILocalInferenceNetworkFactory {
public:
    virtual ~ILocalInferenceNetworkFactory() = default;

    virtual std::unique_ptr<QNetworkAccessManager> createManager() const = 0;
    virtual OperationDeadline::Clock monotonicClock() const {
        return {};
    }
};

} // namespace sentinel::core
