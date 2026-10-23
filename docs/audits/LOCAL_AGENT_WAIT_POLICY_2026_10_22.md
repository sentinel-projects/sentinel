# Local Agent generation wait policy — 2026-10-22

## Trigger and change

A real LM Studio `qwen/qwen3.5-9b` task failed before any tool step because observation classification imposed 60000 ms. Planning independently imposed 120000 ms.

`AgentInferencePolicy` uses the immutable provider registry and the run's existing ModelBinding. Registered Local providers add no classification or planning wall-clock deadline (zero). Cloud/unknown providers retain 60000/120000 ms. An explicitly configured provider timeout remains effective; the production default is zero. No provider/model is selected or redirected by this policy.

Local planning uses the existing cancellable non-streaming request contract. It never misrepresents an unlimited wait as bounded streaming. Both requests preserve the runtime cancellation token. Output/context admission, bounded repairs, step limits, approvals, filesystem authorization and accepted-final checks remain enforced.

## Validation

- Regression cases cover LM Studio, Ollama, llama.cpp, local compatible, Cloud and unknown identities for both phases, including cancellation-token propagation and output budgets.
- Existing real HTTP zero-timeout cancellation test remains in the full suite.
- Tests preset configure/build and Release daemon/Desktop build passed.
- Final full-suite and native LM Studio evidence: pending.
