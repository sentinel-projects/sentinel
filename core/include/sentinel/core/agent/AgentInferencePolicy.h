// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/provider/ProviderRegistry.h"

namespace sentinel::core {
// Local generation may include slow model loading and reasoning. Zero disables
// the wall-clock deadline, not cancellation, token admission or tool safeguards.
inline int agentInferenceDeadlineMs(const ModelBinding& binding, int remoteDeadlineMs) {
    const auto* provider = ProviderRegistry::builtins().find(binding.providerId);
    return provider && provider->kind == ProviderKind::Local ? 0 : remoteDeadlineMs;
}
} // namespace sentinel::core
