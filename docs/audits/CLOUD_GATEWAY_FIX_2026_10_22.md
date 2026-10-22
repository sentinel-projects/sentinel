# Custom cloud gateway and credential refresh — 2026-10-22

## Problem and changes

The Cloud facade displayed a custom endpoint but ModelService and background discovery still used the selected named provider's built-in address. Discovery also constructed a separate settings owner at the default path inside its worker, bypassing the configured profile. After a credential write, the daemon did not immediately refresh discovery.

Cloud with the OpenAI protocol now honors its configured gateway, while explicitly named providers retain their built-in addresses. Discovery captures ModelService's endpoint/credential configuration on its owner thread and uses the same configuration as generation. Model-list URLs preserve path prefixes and avoid duplicating `/v1`. Existing HTTPS and credential checks remain in force.

Endpoint, selected cloud provider, and credential changes invalidate cloud catalogs and health with a sequence fence against old observations. Successful daemon credential writes refresh discovery immediately. Desktop cloud setters/readbacks use the authoritative IPC settings, and provider-state queries are invalidated on cloud configuration changes. The UI labels the custom gateway credential as Cloud API Key, clears the input after submission, and distinguishes stored credentials from verified availability. Safe discovery summaries distinguish HTTP 401 from HTTP 403 without exposing upstream bodies or secrets.

## Validation

- Full tests preset configure/build and Release desktop/daemon build completed. `TMPDIR=/tmp QT_QPA_PLATFORM=offscreen ctest --preset tests --output-on-failure`: **180/180 passed**, 306.82 seconds. The run required normal macOS/local-socket access outside the tool sandbox; `/tmp` avoids the already tracked long socket path issue #28.
- Regression coverage: custom endpoint versus named-provider isolation; immutable captured configurations; stale discovery fencing after endpoint/key changes; rejected non-HTTPS remote endpoints; path-prefix and trailing-slash URL handling; HTTP 403 explanation; daemon IPC credential refresh followed by discovery and generation against a scripted HTTPS gateway.
- QML lint, changed-line C++ formatting, personal-path scan and diff whitespace checks.
- Real Desktop Settings → Models & Providers flow inspected through native accessibility. Stored-key feedback and ready state were observed; secret contents were never displayed. The disposable navigation smoke passed at compact and wide window sizes.

The PR also exposed an existing Fedora metadata CI bootstrap omission: the job invoked a Python validator without installing `python3`. Its validation-tool dependencies now include Python. Actionlint and the packaging consistency validator pass locally; remote Fedora execution remains a separate CI result.

## Live Evren evidence and limits

The configured endpoint was `https://evren-llmapi.ssyz.org.tr/v1`. Initial direct requests using both documented authentication headers returned HTTP 403 with `terms_not_accepted`. The human user read and accepted the provider's terms in Evren; Sentinel did not accept them automatically. After that action both methods returned HTTP 200 and 16 catalog entries. Sentinel's own daemon then reported an available catalog with the same 16 model IDs.

A new synthetic Chat session using provider `cloud-api`, model alias `auto`, requested only `SENTINEL_CLOUD_OK`. The real daemon emitted three ordered `output.delta` events and a successful `run.completed` containing that exact response. No project files or personal chat history were supplied for this test. `auto` is a provider routing alias, so this does not certify a fixed underlying model. A separate no-tool AgentRuntime run completed with the accepted final answer `SENTINEL_AGENT_CLOUD_OK`. A separate streaming Chat was interrupted through the real CLI Ctrl+C path after its first delta: exactly one `run.cancelled` terminal event was received and the CLI exited 130. Live tool execution remains unverified; this evidence does not establish native-tool, vision, structured-output, or all-model parity.

The generic gateway currently uses the existing OpenAI credential slot when OpenAI is the selected Cloud protocol; the UI makes the endpoint association explicit. Named native providers keep their own endpoint and credential slots.
