# RHI production readiness: R18 verification

Status: implemented 2026-10-03.

Reflection rejects malformed SPIR-V framing before calling the bundled reflector, frees every successfully created reflection module, and publishes metadata only after all stages validate. Unsupported descriptors, multiple push-constant blocks and unsupported specialization types return failure with a diagnostic. A failed shader releases its RHI allocation and returns null. Cached descriptor types are checked before array indexing. Push-constant reflection accepts valid stage-specific layouts by covering their combined range.

Shaders retain owned SPIR-V and reflection metadata. The native specialization regression changes the source filename to a nonexistent file and still creates a specialized shader from the retained inputs. Corrupt shader input and invalid cached descriptors return null; parser regressions cover the other unsupported metadata cases.

The reflection memory probe ran `ShaderReflectionTests.SceneShadersKeepEveryBindingInItsDescriptorSet` 10,000 times in each configuration, asserting that all 10,000 tests ran and passed. Process private-byte samples after the first quarter of each run stayed below 5.44 MiB in Debug and 4.67 MiB in Release, without growth across the run. This measures reflection allocations, not total graphics-driver memory. Native shader construction/destruction and partial-object cleanup are covered by the integration suite; no general driver-memory stability claim is made.

Commands, samples and logs are in `build/error-handling-completion/reflection-memory.json` and the adjacent `*-reflection-memory.log` files. See [shared verification](RHIErrorHandlingVerification.md) for builds, native tests and smoke runs.
