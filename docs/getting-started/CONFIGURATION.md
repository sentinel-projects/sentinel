# Configuration

Desktop settings own provider selection, model selection, workspace preferences, permissions, speech, privacy, and system settings. `SettingsService` composes settings snapshots; subsystem services retain ownership of their own state.

Configure an Ollama endpoint before selecting a local model. Cloud providers require an explicit credential and are subject to the effective network policy. A healthy local loopback runtime remains available in Local Only and Offline modes where the relevant policy permits it.

Provider credentials are represented as presence metadata at the UI boundary; see [Secrets](../security/SECRETS.md). Exact stable settings are listed in the [configuration reference](../reference/CONFIGURATION_REFERENCE.md).

## Chat waiting policy

Settings → Models & Providers (AI) → Chat response waiting exposes Compatibility, Extended and Custom for supported OpenAI-compatible text streams, including LM Studio and llama.cpp. Compatibility preserves existing behavior: Chat has a 120-second request total; the zero local-provider setting is a separate preference. Agent classification/planning and approvals have their own limits.

Extended asks you to choose positive initial-response, useful-progress idle and finite total limits. Custom also permits zero initial/idle to disable that check; total stays finite. Values use seconds (a decimal point, up to three decimal places). No hardware-derived preset is guessed. Parsed visible text resets idle; headers and heartbeats do not. Stop, context/token/resource limits and shorter explicit provider/parent ceilings still apply.

The card shows effective values for the model selected above; workspace model overrides are checked again at actual request admission. Preferences apply to the next request, not a running one. Unsupported Ollama/native Claude/Gemini or unknown paths only offer Compatibility; switching to an unsupported model with an explicit saved profile requires resetting it or choosing a supported stream. Structured Agent requests do not inherit Chat profiles.

Wait for save confirmation. If connection or acknowledgement is lost, inspect the stored value after reconnecting; Sentinel does not replay the change automatically. Reset only restores this profile, leaving existing provider preferences intact. An older daemon may not expose these settings. See the [AT-05 result](../audits/P1_P2_AT05_ADAPTIVE_TIMEOUT_SETTINGS_RESULT.md) for versioning, rollback and backend limitations.
