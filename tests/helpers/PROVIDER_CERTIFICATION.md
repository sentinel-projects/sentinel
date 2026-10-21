# Provider certification invocation contract

This opt-in live helper requires `SENTINEL_CERTIFICATION_WORKSPACE_ROOT` before any provider setup. Set it to the canonical absolute path of the exact existing readable fixture workspace you authorize. Positional provider/model/prompt/mode arguments are unchanged. There is no cwd, HOME, repository, or personal-path fallback. Missing, relative, nonexistent, file, symlink/aliased, filesystem-root and whole-home inputs fail closed with exit 2.

The validated canonical directory is passed unchanged as `workspaceContext.rootPath`. Existing tool restrictions, approval, sandbox and external-directory authorization remain authoritative. Setting this variable grants no additional external access. Use the same canonical fixture root to preserve the prior workspace boundary; this does not certify a different fixture's contents or a live provider's behavior. Run local root/gate regression tests before a separately authorized live certification.

## Prose-final probe

The opt-in Agent path accepts `prose-final` as its mode argument. It restricts the run to `read-file`, asks the selected model to inspect the fixture, and exits nonzero when the accepted final is empty or is still a raw canonical object (`source`, `explanations`, or `operations`). This is a live diagnostic for the presentation rule in `docs/concepts/AGENT_RUNTIME.md`; it does not change AC-P1-02's three-repetition evidence requirement or mark any acceptance criterion as passed.

Example, after the user has authorized the local fixture and model:

```bash
SENTINEL_CERTIFICATION_WORKSPACE_ROOT=/absolute/authorized/fixture \
  ./build/tests/provider_certification_probe llama-cpp-server <model> \
  "Read CMakeLists.txt and explain the build in one short paragraph." prose-final
```

Record every attempt, the selected provider/model, and the exact fixture; unavailable models or failed runs leave real-model acceptance blocked.
