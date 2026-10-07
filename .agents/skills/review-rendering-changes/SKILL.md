---
name: review-rendering-changes
description: "Review and refine local C++ rendering-engine code changes, especially ZenEngine, against the surrounding codebase and any relevant implementation plan. Use for reviewing diffs or fixing and simplifying an existing change with concrete evidence, correct layer boundaries, and the user's C++ rules."
---

# Review Rendering Changes

Review the actual change, establish what is wrong or unnecessarily complex, and make the smallest justified correction when refinement is requested. Preserve rendering correctness, ownership, and layer boundaries. Do not invent problems, requirements, or abstractions to make a review look productive.

## Scope and mode

- For review/check/audit requests, including a bare skill invocation, report findings without editing source or documentation. Run appropriate existing checks; keep generated output outside tracked source. Propose a probe if confirming an issue would require source edits.
- For review-and-refine, fix, improve, or simplify requests, review and apply justified corrections within the requested change. Existing authorization to fix is sufficient; do not ask for approval for every routine patch.
- Use the requested repository, files, commit range, and plan. Otherwise review staged and unstaged changes plus relevant untracked source and docs. Inspect both index and working-tree diffs so partially staged edits are understood. For branch reviews, resolve the requested base and merge base; never guess a base branch or silently substitute the latest commit when the working tree is clean.
- Preserve the user's existing edits and staging. Do not reset, discard, stage, commit, or expand into unrelated cleanup unless requested. An adjacent file may need a minimal edit to keep an affected contract consistent.
- A missing plan does not block review. Ask only when unavailable scope or conflicting intended behavior materially changes the next action; continue independent inspection in the meantime.

## Evidence standard

Never treat an assumption as a fact. Base findings and refinements on inspected code, measured behavior, debug logs, or targeted probes.

- Read complete affected functions, their callers, ownership paths, and relevant definitions. A symbol name, comment, familiar engine pattern, or diff fragment is insufficient evidence of behavior.
- Treat plans and docs as intended behavior, and code and runtime observations as implementation evidence. Identify stale or conflicting documentation rather than silently deciding that either the code or the plan must be correct. An active requirement is not permission to implement deferred plan phases.
- A finding needs a concrete trigger or reachable execution path, a violated requirement or invariant, and a consequence. Link the exact code and, when used, the test/log/capture that establishes it. Distinguish defects introduced by the change from pre-existing issues exposed by it.
- Static reasoning is sufficient when the inspected control flow, data flow, or lifetime proves the issue. A concurrency finding can be established by a reachable interleaving and the absence of the required ordering; a successful runtime reproduction is not mandatory.
- When behavior is uncertain, inspect the missing contract first. If runtime evidence is necessary and edits are authorized, use the smallest focused probe or reproduction. Prefer existing diagnostics. Record the question, configuration, result, and limitation; remove only temporary instrumentation added for this investigation unless it has a concrete continuing use.
- Use authoritative API/specification documentation when an external contract needs verification, and connect it to the actual code path and enabled features. Do not invent Vulkan guarantees or infer them from a clean validation run.
- Performance claims require comparable measurements: workload, build/configuration, device, metric, and baseline versus changed result. CPU submission time is not GPU execution time. An extra allocation or wait visible in code is a structural observation, not a measured frame-time regression.
- Keep unverified concerns separate from confirmed findings. Do not patch based solely on a speculative future caller, possible future backend, or guessed failure. If evidence cannot be obtained, state the specific gap and avoid claiming correctness or performance was verified.

## Review the change in context

1. Read applicable `AGENTS.md`, the user's current rules, and `.clang-format`. Establish the initial working-tree state and requested diff. Locate the active implementation, relevant tests, and build commands from the repository.
2. Read a supplied plan and narrowly search for related design or verification docs when needed. Extract the intended behavior, constraints, and acceptance criteria for the current scope. Do not turn a small review into a new planning exercise.
3. Trace the affected path across its actual layers. Identify who owns policy, state, resource lifetime, command recording, submission, and native API execution. Follow headers, implementations, call sites, and dependencies rather than imposing a preferred architecture.
4. Inspect correctness first, then layering, avoidable complexity/duplication, and compliance with the C++ rules. Examine only domain concerns affected by the change; the list below is a guide, not a mandatory full-engine audit.
5. Verify each candidate issue against callers and existing protections before reporting or fixing it. Stop expanding the investigation once the relevant contracts and consequences are established.

For ZenEngine, use these as search starting points, and verify that they still describe the checkout:

- `ZenCore/Include/Graphics/RenderCore/V2` and its source implementation: rendering policy, render-graph work, frame/resource management.
- `ZenCore/Include/Graphics/RHI` and its source implementation: backend-facing interfaces and contracts; consult the local RHI `README.md` when relevant.
- `ZenCore/Include/Graphics/VulkanRHI` and its source implementation: native Vulkan mechanisms.
- `ZenCore/Include/Templates`: existing containers and utilities. `Doc`, `ZenSamples`, and the CMake files: plans, verification, tests, and build configuration.

Do not assume V2 or a particular path is active merely because it exists. Do not copy today's implementation details into universal rules.

### Rendering concerns, when affected

- **Lifetime:** ownership from recording through submission and actual GPU completion; deferred destruction, reuse, retained references, cancellation, and teardown. CPU scope exit or command-list reset alone does not establish GPU completion.
- **Ordering and state:** render-graph dependencies, accesses and barriers, image/subresource layouts, queue ownership, semaphore/fence use, and CPU thread ownership. Inspect the complete producer-to-consumer path.
- **Bindings and data:** descriptor/resource validity, shader interface and CPU/GPU layout agreement, offsets, bounds, alignment, formats, usage flags, and stale cached state.
- **Failures:** partial initialization, rejected submissions, unsupported capabilities, and recovery/cleanup. Verify that failure cannot publish invalid resources, commit success state, or allow later work to use invalid data.
- **Frame and resource transitions:** resize/recreation, frames in flight, reset/reuse, and resource replacement where the patch changes those paths.
- **Cost:** changed allocations, copies, synchronization, submissions, or cache behavior on an actual hot path. Measure before claiming an optimization.

## Refine without overimplementing

Make every edit answer a demonstrated problem, an active requirement, a concrete duplication, or an explicit programming rule. Leave already-correct direct code alone.

- Fix the cause in the layer that owns it. Keep high-level rendering policy out of native backend mechanics, and backend details out of general interfaces unless the existing contract requires them. Do not bypass a boundary to make a local patch shorter.
- Reuse a suitable existing type, container, helper, or error path before introducing another. Inspect its contract; do not reuse by name alone.
- Keep straightforward logic local. Extract shared logic when duplication could cause inconsistent behavior, or when a function isolates a coherent responsibility. Avoid one-call forwarding helpers, manager classes, policy frameworks, and generic extension points without a current need.
- Add a type only when it expresses necessary ownership, state, or an invariant more clearly than existing types. Fewer types/functions is a preference, not a reason to combine layers, erase meaningful distinctions, or weaken correctness.
- Keep one authoritative representation of state where possible. Do not introduce duplicated flags, caches, conversions, or parallel implementations without evidence that they are needed.
- Avoid speculative checks, fallbacks, retries, compatibility paths, and future features. Preserve validation required at real input/API boundaries and handling of actual failures.
- Use the narrowest complete fix. Do not remove synchronization, lifetime protection, validation, or cleanup merely to reduce code. Do not add a global wait to hide an untraced dependency or lifetime bug; establish the required ordering and assess any intentional serialization.
- Update directly affected documentation when the fix changes a documented contract. Do not rewrite a plan to conceal an implementation mismatch.

### Global C++ programming rules

Apply these rules to all C++ written or modified under this skill, including tests and probes. Read current user/global instructions on every invocation and incorporate any later additions; this list does not freeze or replace those instructions.

1. Use a single return statement per function, at the end; avoid early returns. Functions that need no explicit return, such as void functions, may omit it.
2. Do not use the `auto` keyword except for iterator types. Use explicit types elsewhere.
3. Use lambdas only when necessary, and keep them short. Use named functions or methods for long logic.
4. Prefer the current project's own containers when a suitable one exists. In ZenEngine, use `HeapVector` instead of `std::vector`; apply the same preference to other available project containers.
5. Format C++ files using the project's `.clang-format` file. VSCode's format-on-save feature can be used to apply this formatting automatically.
6. Reduce code duplication, including duplicated functions and duplicated logic. Reuse existing code or extract shared logic into suitable functions or methods.
7. Do not throw exceptions for error handling. Use error codes or status results, log error messages, and use assertions for violated invariants as appropriate. Handle failures explicitly so execution does not continue with invalid data.

Do not copy a rule violation simply because adjacent legacy code uses it. Keep compliance work within the touched functions and necessary dependencies. For recoverable runtime failures, an assertion alone is insufficient when it disappears in release builds; propagate or handle status and keep later operations gated on success while preserving the single final return.

## Verify and report

- Select checks from the affected contracts and repository setup. Build the affected targets and run relevant existing tests where available. Add or adjust a regression test when it demonstrates a meaningful behavior or invariant; do not add tests that merely mirror code structure or cover a cosmetic edit.
- For GPU behavior, use the relevant sample/integration test, validation output, capture, or focused probe when needed. A successful build or CPU unit test does not prove device-side synchronization, rendered output, or performance. Inspect the right evidence for the claim.
- Format changed C++ with the repository configuration and inspect the final diff for unintended churn, style violations, and broken callers. Distinguish your refinements from the user's starting changes.
- State which checks actually ran and what they establish. If a device, toolchain, fixture, or other dependency is unavailable, report that limit; never describe an unrun check as passed.
- Stop when the scoped findings are addressed and relevant checks pass, or when the remaining evidence gap is clearly identified. Repeat checks only after relevant edits, failures, or new evidence; do not keep refactoring to fill time.

Keep the final response concise and useful for a decision:

- Present confirmed unresolved findings by severity, each with a code location, triggering condition, consequence, and supporting evidence. Label rule/structure issues accurately rather than presenting them as runtime bugs.
- If refining, summarize the fixes and why they were necessary, then report remaining issues. Do not present an already-fixed issue as still open.
- Report verification results and material limitations. Mention plan mismatches and unresolved questions only when they affect the requested change.
- If no actionable findings remain, say so directly, with the verification scope. Do not manufacture suggestions or claim that a finite review guarantees the absence of bugs.
