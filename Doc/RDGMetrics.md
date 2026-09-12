# RDG metrics

RenderCore V2's `RDGExecutor` owns an `RDGMetrics` collector. Both frame graphs and
standalone graphs reach it through the existing `Execute()` path. No renderer changes
are needed. The default sink writes through spdlog at info level.

The first execution is sampled, then sampling is considered every 120 executions,
with at least five seconds between automatic reports. Frame/mixed graphs and
transfer-only graphs have separate streams **on the executor**, so creating a new
upload graph or changing its name does not reset throttling. Transfer-only reports
contain totals; individual transfer nodes and their diagnostic details are hidden by
default. Transfer nodes in mixed graphs also contribute to totals.

## Configuration

```cpp
auto& metrics = renderDevice.GetRDGMetrics();
auto options = metrics.GetOptions();
options.logging.sampleEvery = 120;
options.logging.minInterval = std::chrono::milliseconds(5000);
options.validate = true;
options.nodeTimings = false;
options.preparationTimings = false;
options.maxNodeDetails = 32;
options.maxDiagnosticDetails = 16;
options.includeOptimizationDetails = false; // Keep optimization counts; omit per-node entries.
metrics.Configure(options);

// Capture the next frame/mixed graph, bypassing cadence and time throttling.
metrics.RequestCapture();

// Include transfer details, then capture the next transfer-only graph.
options.includeTransferNodes = true;
metrics.Configure(options);
metrics.RequestCapture(true);
```

`Configure()` resets both streams' cadence and pending window counts. To inspect
every execution temporarily, set `sampleEvery = 1` and `minInterval = 0ms`.
Set `options.logging.enabled = false` to disable collection and logging, including
requested captures. Set `validate = false` for counters without diagnostic replay.
When collection and validation are enabled, access/barrier validation history is
updated on every execution, including unsampled graphs. Only reports are sampled.
Disabling either collection or validation clears history; re-enabling starts from
current import state. Changing cadence or detail limits preserves continuous history.
`RDGExecutor::GetMetrics()` exposes the same interface for standalone executors.

To send structured samples to a file, UI, or another collector:

```cpp
metrics.SetSink([](const zen::rc::RDGMetricsSnapshot& sample) {
    // Copy sample if it will be retained after the callback.
    SaveMetrics(sample);
});

// Keep the last structured sample, but perform no log I/O.
metrics.SetSink({});
const auto& last = metrics.GetLastSnapshot();
```

The last snapshot survives graph rebuilds and graph destruction; it owns its labels
and stores stable resource IDs rather than resource pointers. It changes at the next
capture. Configuration, execution, and sink access belong on the executor's thread.
Sinks run synchronously; a slow sink can still stall a captured frame.

## Reading a report

`window(executions,nodes)` counts **all** executions/declared nodes in that stream since the
previous sample, including the sampled execution. Other counters describe **only the
sampled graph**. They are not totals or averages across unsampled frames.

| Field | Meaning |
| --- | --- |
| `nodes`, `graphics`, `compute`, `transfer` | Live scheduled pass counts by declared type, not actual queue assignment. Culled passes do not execute. |
| `resources`, `imported`, `transient` | Live logical resources, imported resources, and non-exported transients. Several logical resources may reuse one physical allocation. |
| `edges` | Unique node dependencies derived from resource versions (producer, overwrite, and old-value reader protection). |
| `dependency_hazards` | Resource dependencies found by compilation; these are not barrier commands. |
| `reordered` | Live nodes whose execution position differs from their position among live recorded nodes. This is informational; IDs are lookup keys, not sorting priorities. |
| `liveness.culled` | Declared passes removed because they have no observable result or preserved side effect. |
| `liveness.reused_allocations` | Live logical allocations assigned a compatible native object already used by an earlier, non-overlapping lifetime. |
| `pool_estimated_bytes.assigned`, `available`, `retiring` | Unique graph-assigned payload bytes, idle cached bytes, and evicted bytes awaiting captured queue serials. These estimates exclude native allocation alignment/overhead and ownership outside the graph/pool. |
| `barriers.calls` | Actual RDG `AddTransitions()` calls, including encoder-internal calls. |
| `buffer`, `texture` | Individual transitions emitted by node prologues. Buffer usage unions can expand one hazard into several transitions. |
| `initial_resources` | First-use resources needing synchronization against persistent state; a resource can emit multiple transitions. |
| `internal_memory`, `internal_texture` | Transitions inside transfer operations and mip generation. Add these to prologue counts for total transition entries. |
| `read`, `read_write` | Declared resource access records, after same-pass accesses have been merged. |
| `compile` | Total CPU preparation for this execution: compilation, queue eligibility, any state refresh after uploads, and the final shader identity check. Excludes upload flushing/waiting, snapshot/rollback copies, diagnostic order validation, submission, and GPU work. An earlier explicit diagnostic `Prepare()` is outside this execution. |
| `preparation_passes` | Full compile/barrier passes used for this execution. Normally 1; intervening uploads or another preparation of the same graph require a refresh and increase this count. |
| `pass_setup_cpu_us(total,bindings,pipeline)` | Optional CPU attribution within `compile`. `total` covers shader pass compilation, parameter construction, layout acquisition, pipeline resolution, geometry and indirect bindings. `bindings` and `pipeline` are subsets of `total`. Pipeline resolution includes native pipeline creation on a cache miss. |
| `pipeline_cache(hits,misses,created,failed,evicted)` | Pipeline-cache activity during this execution's automatic preparation, including refreshes. Misses include failed creation attempts. Replaying an unchanged compiled graph makes no pipeline requests. |
| `pipeline_cpu_us(key,lookup,create)` | Optional CPU attribution within pass setup's `pipeline` time. Key construction, cache lookup/equality/LRU update, and miss-side creation are measured separately. Creation includes specialization shader preparation and the RHI creation call; it excludes cache insertion/eviction and GPU execution. |
| `precompiled` | The graph was already compiled before this execution began preparing. First device execution normally reports false; replay or an earlier explicit `Prepare()` reports true. |
| `execute` | CPU graph command recording and state tracking, including sampled diagnostics; excludes report formatting, sink I/O, submission, and GPU work. |

Phase 6B moved automatic preparation into this timing. Compare end-to-end CPU measurements across the change; the old `compile` counter omitted the device’s preliminary preparation and queue-selection work.

Phase 6C adds `options.preparationTimings`. It defaults off, and the report prints
`pass_setup_cpu_us=disabled`; structured samples have `passCompileTimings.enabled=false`.
When enabled with collection, each shader compilation is timed, including preparation
on executions that will not be sampled: preparation precedes the sampling decision.
Automatic refreshes accumulate into the execution's sample. An unchanged graph replay
does not recompile shader passes, so enabled setup timers correctly report zero. An
earlier explicit diagnostic `Prepare()` is outside the execution's measurements.
Binding and pipeline times are included in both setup total and `compile`; do not add
them together as separate phases. These are CPU timings, with no GPU queries or waits.

Phase 6D adds pipeline-cache counters and CPU attribution. Counters remain available
when preparation timing is off; `pipeline_cpu_us=disabled` distinguishes that state
from measured zero creation time on a cache hit. A standalone executor uses its own
timing option, independently of the device executor's configuration. These pipeline
times are subsets of `pass_setup_cpu_us.pipeline`, not additional execution stages.

`RenderDevice::GetPipelineCacheMetrics()` returns device-lifetime counters, including
direct cache requests and failed graph preparations. Copy the struct and use
`current.Since(previous)` for an interval. `invalidations` counts resize cache clears;
`invalidatedEntries` counts entries removed by them. These clears normally happen
outside graph execution and are therefore visible in device totals. Shader replacement
creates a new native identity and thus a cache miss; it does not eagerly purge old
entries. The existing 256-entry LRU and completion-aware retirement bound and release
them. `timedRequests` reports how many requests contributed CPU measurements, since
lifetime counters can span changes to timing options. The counters are cumulative and
are not rolled back when graph recording fails; no thread synchronization is added.

Pipeline keys compare canonical field values, shader stable identity/generation, and
sorted specialization overrides. Hash collisions cannot substitute another pipeline.
Dynamic rendering keys omit per-pass load/store operations, texture pointers and render
areas; legacy render-pass keys retain load/store distinctions, and the rendering mode
is part of the key. Existing resize invalidation and graph shader-identity checks stay
in place. Renderer binding interfaces are unchanged.

Node details identify recording ID, actual execution position, pass tag/type,
access counts, transition counts, and source/destination pipeline masks. Optional
`record_cpu_us` measures each node's CPU recording, barriers, diagnostics, and state
tracking. Per-node timing defaults off: the log displays `record_cpu_us=disabled`,
and structured snapshots have `nodeTimingsEnabled=false` (their timing values are
unmeasured). Set `options.nodeTimings = true` before `metrics.Configure(options)`
to measure sampled nodes. Enabled values are printed to one decimal place, so very
short measurements can round to `0.0`. This is CPU command recording, not GPU pass
execution; no GPU timing queries or waits are introduced.

Compiled shader passes reuse cleared CPU storage across graph rebuilds. Idle storage
is bounded internally to **256 objects and 1 MiB of object/vector payload per graph**,
whichever limit is reached first. This is separate from the native resource pool and
its `pool_estimated_bytes` counters. The bound excludes allocator headers and pointer
list capacity; it does not constrain active passes or command-owned parameter copies.
Oversize idle entries are deleted, and graph destruction or frame-graph resize trims
the cache. Every reuse resolves current shader, resource, geometry, and indirect
bindings. No pipeline, rendering-layout, or resource ownership is held by idle passes.

Details are capped independently of counters. `details_omitted` counts eligible
entries excluded by detail limits or transfer-detail settings. Optimization candidates
are summarized by default and do not consume the diagnostic budget or increase
`omittedDiagnostics`. Totals still include every finding. When optimization details
are enabled, later correctness/unknown-state diagnostics replace optimization entries
if the detail budget is full. Names in text output are bounded and kept to one line.

Sorting and order validation use the same version declarations for all resources. `End()` resolves base handles and raw bindings into automatic versions in pass declaration order; explicit handles already select their value and support forward producer references. The independent order validator reconstructs producer and overwrite constraints from those declarations, without trusting compiler edges. Read-only texture layout differences do not impose value dependencies: barrier validation checks the transitions in the actual execution order. A high `reordered` count alone is not a correctness diagnostic.

## Internal pool and liveness controls

Configure these inside RenderCore/RDG; renderer resource/binding interfaces are unchanged.

```cpp
auto* resources = graph.GetResourceManager();
resources->SetPoolConfig({256ull * 1024 * 1024, 120}); // Idle byte budget and max idle build generations.
const auto pool = resources->GetPoolStats();
const auto buckets = resources->GetPoolBuckets();
resources->TrimPool(true); // Trim every currently available allocation; active resources stay owned.

// Optional diagnostic comparison, before Begin(). Defaults are true/true.
graph.SetOptimizations(true, true);
```

Configuration/trim calls return `bool` and report lifecycle errors through the graph's
existing error/log path; check the return values in production callers. The byte budget
limits retained idle payload, not total GPU memory. Texture estimates include mips,
layers, depth, samples, and format size. Active graph allocations, extracted owners,
native overhead, and deferred retirement can exceed that budget. Eviction uses the
device's completion-aware release path, preserving physical state until final release.
Resize resets the frame graph and trims its idle pool. Idle age advances with the
manager build generation on `Reset`/`Begin`, including an explicit reset before a new build.

`GetPoolStats()` also exposes hit/miss/eviction counts, assigned/available allocation
counts, descriptor count, and conservative in-flight bytes. Assigned allocations are
conservatively counted in flight while submitted queue work is outstanding; available
entries use the serials captured when returned. Retiring-byte accounting ends when the
captured serials complete; actual device frame-slot cleanup can release memory later.
`GetPoolBuckets()` describes available counts and bytes for each exact descriptor.

Imported writes and extractions are always liveness roots. Built-in transfer passes are
otherwise cullable; `NeverCull()` preserves an explicit side effect. Shader callbacks
remain roots by default because their CPU effects cannot be inferred from reflection.
An internal caller can set a shader pass description's `allowCulling = true` only when
its effects are confined to declared GPU resources. All declarations still undergo
structural/version validation, including dead passes; content validation and execution
apply to the live schedule. Prior writers are retained conservatively for partial writes.

First/last use covers the whole logical allocation and all its versions/views. Reuse
requires exact descriptor equality and strictly non-overlapping lifetimes; imported and
extracted allocations are excluded. Each new logical transient starts undefined even
when it reuses native storage. Physical layout/access history survives reuse and replay.
Graphs and their pools execute through one executor state tracker across rebuilds;
changing execution trackers is a logged lifecycle error. This does not implement
range-aware synchronization or backend memory aliasing.

## Diagnostics

The checker independently replays declared accesses against the transitions that
RDG actually emits. It does not reuse compiler dependency decisions to
decide whether synchronization is required. It checks:

- Missing synchronization for write/read, write/write, read/write, and layout changes.
- Source/destination stage coverage, write availability/read visibility, texture
  layouts, and transition range coverage.
- Resource-specific stages from shader reflection and fixed-function/transfer usage.
  A pass's depth tests or indirect arguments do not add stage requirements to its
  other bindings. Merged indirect/shader accesses are checked at their own stages.
  Logical pipeline order widens execution scopes only, not memory access scopes.
- Writer visibility across successive readers, including readers at different
  stages. It retains visibility per destination stage/access instead of losing the
  writer when the first reader executes. This history spans graph executions and
  includes unsampled frame/transfer graphs and staging uploads.
- Dependency ordering reconstructed from recording-order accesses, plus duplicate,
  missing, and invalid node IDs in the compiled schedule.
- Reads of graph-created resources before a producer.

`redundant_barrier_candidate` and `broad_texture_range` identify optimization
candidates, not instructions to remove barriers. Whole-image transitions are
currently conservative by design. Each cubemap face/mip copy can therefore contribute
one `broad_texture_range` finding. These counts appear together as
`optimization_candidates(...)` in the summary. Set `includeOptimizationDetails = true`
to include individual `optimization=...` entries; `RequestCapture()` alone does not
enable them. This reporting option does not change synchronization or validation.
`unknown_import_state` means RDG has no tracked
prior access or explicit host-initialization declaration; undeclared external
initialization may still be valid. The staging upload queue imports its CPU-filled
sources with `ImportHostWrittenBuffer()`. This graph-local contract requires every
read range to be initialized and its host writes made available before submission,
with no overlap with in-flight GPU accesses. Staging allocations use coherent memory
and protect outstanding ranges until completion. Vulkan's
[host-write submission guarantees](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
make these writes visible without an extra source-buffer barrier. This declaration
does not fabricate a GPU access or discard tracked GPU writers; subsequent GPU
hazards remain checked. Ordinary imports and textures retain unknown-state checks.
Detailed diagnostics include node IDs/names and resource stable
IDs/names so findings can be traced to declarations. `previous_node=-1` denotes
external state or a predecessor in an earlier graph; node IDs are local to a graph.
External tracker updates/invalidation and resource retirement forget the affected
history, including staging-block eviction. Owners bypassing RenderDevice resource
management must notify its external-state invalidation API before retirement.

The compiler now retains writer visibility independently of metrics. A transfer
write followed by a transfer reader and a vertex-input reader emits the additional
visibility barrier required by the vertex reader, including across unsampled
uploads and graph executions. Repeated covered readers remain barrier-free, and
new writes reset coverage. Regressions assert the emitted barriers and require no
missing/stage/access diagnostics; isolated validator tests still inject and detect
missing or corrupt synchronization. Barrier normalization includes retained writer
source access without changing the texture's current old layout. Empty initial buffer
states no longer emit barriers: the first access establishes state, and later
hazards still synchronize normally. This removes first-write redundant-barrier
candidates without suppressing unknown external-read diagnostics.

Validation uses RDG's existing whole-resource declarations and persistent import
state. It cannot prove shader declarations complete, validate intra-pass operations
(their barriers are counted), recover writer history from periods when validation
was disabled, or verify backend queue ownership/semaphore/submission behavior.
Unknown external state remains a limitation. No diagnostics is not a guarantee of
GPU correctness. A logical write
declared as `eReadWrite` is treated as a producer; attachment `Load` without valid
contents needs more precise declaration metadata to diagnose separately.

## Reusing the logger

`Utils/MetricsLogger.h` contains a header-only `MetricsLogger<Sample>` with no RDG,
RHI, or spdlog dependency. Other systems define their own sample structure and sink:

```cpp
struct CacheSample { uint64_t hits; uint64_t misses; };
zen::MetricsLogger<CacheSample> logger;
logger.SetSink([](const CacheSample& sample) { SaveCacheMetrics(sample); });
if (logger.TryBeginSample())
{
    logger.Publish(CacheSample{hits, misses});
}
```

The logger's sampling decision performs no allocation, formatting, or locking, and
avoids clock reads on most rejected calls. With `validate=false`, RDG's unsampled
path adds stream counters and null checks. With validation enabled, normalized
access/barrier replay also runs between captures to preserve writer visibility;
this adds CPU work even when no report is emitted. Label copies, report counters,
ordering diagnostics, and optional per-node clocks remain sampled. The collector
retains bounded output storage; validation history scales with tracked resources
and is removed on notified retirement. Scratch storage scales with node barriers.

## Tests

```sh
cmake --build build/arm64-apple-clang-debug --target RenderCoreTest ZenCore -j 4
./bin/RenderCoreTest
```

Tests cover cadence, explicit captures, disablement, throttling across short-lived
graphs, independent reporting streams, capped details, rebuild/re-execution,
actual mock-RHI transition counts, mip/transfer internal barriers, missing or
corrupt synchronization, buffer usage unions, layout/range coverage, and ordering.
