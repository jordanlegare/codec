# CODEC agent instructions

These instructions apply to the entire repository. Start every run from repository evidence, not from remembered conversation state.

## Machine-readable contract

```yaml
contract_version: 1
architecture_manifest: README.md
execution_workflow: AI_WORKSHEET.md
runtime_truth: [code, tests, capabilities, cmake, changelog]
truth_classes: [S0, S1, D]
core_direction: stream-first
profile_boundary: profile-specific semantics stay outside generic core
continuity_evidence: [git_head, open_prs, exact_head_ci, roadmap_issue]
roadmap_issue_title: CODEC v1.0 roadmap execution log
```

## Bootstrap

1. Record the checked-out branch, exact HEAD SHA, and working-tree status.
2. Read the architectural manifest in [`README.md`](README.md).
3. Fill the work record and proof contract in [`AI_WORKSHEET.md`](AI_WORKSHEET.md) before editing.
4. Verify current behavior from `CMakeLists.txt`, relevant public headers, implementation, tests, CLI capability output, and [`CHANGELOG.md`](CHANGELOG.md).
5. When GitHub access is available, inspect open pull requests, CI on the exact current head, and the issue whose title exactly matches `CODEC v1.0 roadmap execution log`. Record unavailable or ambiguous continuity sources instead of guessing.

Do not resume a task merely because a previous chat said it was active. Determine the active roadmap stage and next unmet evidence gate from the current tree and GitHub state. Do not overwrite unrelated or concurrent work.

## Implementation contract

- Complete one bounded change class from the worksheet at a time.
- Follow the README roadmap order; do not skip an unmet predecessor gate.
- Prefer generic `Stream*` primitives. Keep PCM, watermark, separation, and identity behavior inside the Audio Stream Profile; existing `Feed*` names may remain only for v0.1 compatibility.
- Preserve accepted S0 before optional interpretation. Keep exact profile-defined S1 separate from provenance-bearing, probabilistic D artifacts.
- Make unavailable capabilities explicit. Never fabricate models, fidelity, identity, recovery, scale, deployment, or release evidence.
- Write the proof first for behavioral changes, then implement the smallest coherent change.
- Use authorized sources only. Do not bypass access controls or add surveillance, adjudication, or autonomous-enforcement authority.

## Required verification

Run the Release and sanitizer commands in the worksheet. Run `./build/codec capabilities` when CLI capability claims are affected. Update the README manifest and changelog only when code and tests prove a capability change.

Before merge, audit the diff and claims, report the exact tested SHA, and require every applicable worksheet merge gate. Automatic merge is allowed only for the exact head SHA whose required CI is green; reverify after any head movement.
