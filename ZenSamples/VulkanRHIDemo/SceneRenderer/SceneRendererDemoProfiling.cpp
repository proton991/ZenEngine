#include "SceneRendererDemoProfiling.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderFrameState.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHIGPUFrameTiming.h"
#include "Platform/ConfigLoader.h"
#include "Templates/HashMap.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <tuple>

namespace zen
{
namespace
{
constexpr size_t MaxProfileFrames = 100000;

constexpr size_t MaxProfileGraphs = 16384;

constexpr size_t MaxProfilePasses = 1000000;

constexpr size_t MaxPendingGPUFrames = 32;

using ProfileFrameSamples = HashMap<std::string, HeapVector<double>>;

struct ProfileFrameSummary
{
    ProfileFrameSamples cpu;

    ProfileFrameSamples gpu;

    HashMap<std::string, uint64_t> unavailable;
};

using ProfileSummaryKey = std::tuple<std::string, std::string, std::string, uint32_t, uint32_t>;

struct ProfilePassSummary
{
    HeapVector<double> cpu;

    HeapVector<double> gpu;

    uint64_t unavailable{0};
};

// Ordered keys keep exported pass statistics deterministic; HashMap has no sorted traversal.
using ProfilePassSummaries = std::map<ProfileSummaryKey, ProfilePassSummary>;

void JSONString(std::ostream& output, std::string_view text)
{
    constexpr char hex[] = "0123456789abcdef";

    output << '"';

    for (const unsigned char ch : text)
    {
        if (ch == '"' || ch == '\\')
        {
            output << '\\' << ch;
        }
        else if (ch < 32)
        {
            output << "\\u00" << hex[ch >> 4] << hex[ch & 15];
        }
        else
        {
            output << ch;
        }
    }

    output << '"';
}

void CSVString(std::ostream& output, std::string_view text)
{
    output << '"';

    for (const char ch : text)
    {
        if (ch == '"')
        {
            output << '"';
        }

        output << ch;
    }

    output << '"';
}

void ConfigureStream(std::ostream& output)
{
    output.imbue(std::locale::classic());

    output << std::setprecision(std::numeric_limits<double>::max_digits10) << std::boolalpha;
}

const char* QueueName(RHICommandContextType queue)
{
    const char* name = "unknown";

    switch (queue)
    {
        case RHICommandContextType::eGraphics: name = "graphics"; break;
        case RHICommandContextType::eAsyncCompute: name = "compute"; break;
        case RHICommandContextType::eTransfer: name = "transfer"; break;
        default: break;
    }

    return name;
}

void FingerprintFile(std::ostream& output, const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);

    const bool opened = file.is_open();

    uint64_t hash = UINT64_C(14695981039346656037);

    uint64_t bytes = 0;

    char block[65536];

    while (file.read(block, sizeof(block)) || file.gcount() > 0)
    {
        const std::streamsize count = file.gcount();

        bytes += static_cast<uint64_t>(count);

        for (std::streamsize i = 0; i < count; ++i)
        {
            hash ^= static_cast<unsigned char>(block[i]);

            hash *= UINT64_C(1099511628211);
        }
    }

    const bool readable = opened && file.eof() && !file.bad();

    std::ostringstream digest;

    digest << std::hex << std::setfill('0') << std::setw(16) << hash;

    output << "{\"path\":";

    JSONString(output, path.generic_string());

    output << ",\"readable\":" << readable << ",\"bytes\":" << bytes << ",\"fnv1a64\":";

    if (readable)
    {
        JSONString(output, digest.str());
    }
    else
    {
        output << "null";
    }

    output << '}';
}

std::string FingerprintInputs(const DemoProfilingOptions& options)
{
    std::ostringstream output;

    ConfigureStream(output);

    std::error_code error;

    output << "{\"algorithm\":\"fnv1a64\",\"executable\":";

    if (!options.arguments.empty())
    {
        FingerprintFile(output, std::filesystem::absolute(options.arguments[0], error));
    }
    else
    {
        output << "null";
    }

    output << ",\"configuration\":";

    FingerprintFile(output, ZEN_CONFIG_PATH);

    output << ",\"scene_document\":";

    FingerprintFile(output, platform::ConfigLoader::GetInstance().GetDefaultGLTFModelPath());

    output
        << ",\"scene_hash_scope\":\"main asset document; external buffers and textures are not included\""
           ",\"shader_scope\":\"all SPIR-V files in the configured shader directory\",\"shaders\":[";

    HeapVector<std::filesystem::path> shaders;

    std::filesystem::recursive_directory_iterator entry(SPV_SHADER_PATH, error), end;

    while (!error && entry != end)
    {
        if (entry->is_regular_file(error) && entry->path().extension() == ".spv")
        {
            shaders.push_back(entry->path());
        }

        entry.increment(error);
    }

    std::sort(shaders.begin(), shaders.end());

    for (size_t i = 0; i < shaders.size(); ++i)
    {
        output << (i == 0 ? "" : ",");

        FingerprintFile(output, shaders[i]);
    }

    output << "],\"shader_enumeration_complete\":" << !error << '}';

    return output.str();
}

void WriteStatistics(std::ostream& output, HeapVector<double>& samples)
{
    output << "{\"count\":" << samples.size();

    if (samples.empty())
    {
        output << ",\"min\":null,\"max\":null,\"mean\":null,\"median\":null,\"p95\":null}";
    }
    else
    {
        std::sort(samples.begin(), samples.end());

        double sum = 0;

        for (const double value : samples)
        {
            sum += value;
        }

        const size_t middle = samples.size() / 2;

        const double median =
            samples.size() % 2 ? samples[middle] : (samples[middle - 1] + samples[middle]) / 2;

        const size_t p95 = (samples.size() * 95 + 99) / 100 - 1;

        output << ",\"min\":" << samples.front() << ",\"max\":" << samples.back()
               << ",\"mean\":" << sum / samples.size() << ",\"median\":" << median
               << ",\"p95\":" << samples[p95] << '}';
    }
}

void WriteVec4(std::ostream& output, const Vec4& value)
{
    output << '[' << value.x << ',' << value.y << ',' << value.z << ',' << value.w << ']';
}
} // namespace

struct SceneRendererProfiling::State
{
    struct Frame
    {
        uint64_t index{UINT64_MAX};

        uint32_t local{0};

        const char* phase{"startup"};

        double cpuUs{0};

        uint64_t rhiStartUs{0};

        uint64_t rhiCPUUs{0};

        RHIExecutionCounters counterStart;

        RHIExecutionCounters counters;

        bool succeeded{false};

        uint32_t width{0}, height{0}, requestedMode{0}, resolvedMode{0};

        const char* method{"none"};

        double startUs{0}, endUs{0};

        RHIGPUFrameTimingPtr gpuTiming;

        RHIGPUTimingStatus gpuStatus{RHIGPUTimingStatus::ePending};

        double gpuUs{0};

        size_t gpuIntervals{0}, gpuExcludedIntervals{0};
    };

    struct Graph
    {
        NameID name;

        uint64_t execution{0}, frame{UINT64_MAX};

        bool transfer{false};

        double compileUs{0}, executeUs{0}, submissionUs{0};

        uint64_t assignedBytes{0}, availableBytes{0}, retiringBytes{0};

        uint32_t nodeCount{0}, omittedNodes{0}, omittedSubmissions{0};
    };

    struct Pass
    {
        size_t graph{0};

        rc::RDGNodeMetrics node;

        uint32_t queueEquivalence{UINT32_MAX};

        bool cpuMeasured{false};
    };

    DemoProfilingOptions options;

    RHIGPUInfo gpu;

    RHIQueueCapabilities queues;

    std::string runID, inputIdentity, configuration, settings;

    HeapVector<Frame> frames;

    HeapVector<size_t> pendingGPUFrames;

    std::chrono::steady_clock::time_point frameOrigin{};

    HeapVector<Graph> graphs;

    HeapVector<Pass> passes;

    Frame current;

    uint64_t droppedFrames{0}, droppedGraphs{0}, droppedPasses{0}, droppedGPUFrames{0};

    bool active{true};

    bool frameOpen{false};

    void CollectGPUFrames(bool abandon = false);

    void RetainCurrentFrame();

    void WriteFramesCSV(std::ostream& output, ProfileFrameSummary& summary) const;

    void WritePassesCSV(std::ostream& output, ProfilePassSummaries& summaries) const;

    void WriteSummaryJSON(std::ostream& output,
                          ProfileFrameSummary& frameSummary,
                          ProfilePassSummaries& summaries,
                          bool runSucceeded,
                          bool unchanged) const;

    void Capture(const rc::RDGMetricsSnapshot& snapshot)
    {
        if (graphs.size() < MaxProfileGraphs)
        {
            const size_t graphIndex = graphs.size();

            graphs.push_back({snapshot.graph, snapshot.execution, snapshot.frameIndex,
                              snapshot.transferOnly, snapshot.compileCPUUs, snapshot.executeCPUUs,
                              snapshot.submissionCPUUs, snapshot.assignedTransientBytes,
                              snapshot.availableTransientBytes, snapshot.retiringTransientBytes,
                              snapshot.nodeCount, snapshot.omittedNodes,
                              snapshot.omittedSubmissionDetails});

            for (const rc::RDGNodeMetrics& node : snapshot.nodes)
            {
                if (passes.size() < MaxProfilePasses)
                {
                    const size_t logical = static_cast<size_t>(node.plannedQueue);

                    uint32_t equivalent =
                        logical < queues.queueIds.size() ? queues.queueIds[logical] : UINT32_MAX;

                    for (const rc::RDGSubmissionMetrics& group : snapshot.submissions)
                    {
                        if (group.id == node.submissionGroup)
                        {
                            equivalent = group.queueEquivalenceId;
                        }
                    }

                    passes.push_back({graphIndex, node, equivalent, snapshot.nodeTimingsEnabled});
                }
                else
                {
                    ++droppedPasses;
                }
            }
        }
        else
        {
            ++droppedGraphs;

            droppedPasses += snapshot.nodes.size();
        }
    }

    const char* Phase(uint64_t frame) const
    {
        const char* phase = frame == UINT64_MAX ? "startup" : "unmatched";

        const Frame* found = std::lower_bound(
            frames.begin(), frames.end(), frame,
            [](const Frame& entry, uint64_t number) { return entry.index < number; });

        if (found != frames.end() && found->index == frame)
        {
            phase = found->phase;
        }

        return phase;
    }
};

void SceneRendererProfiling::State::CollectGPUFrames(bool abandon)
{
    size_t pending = 0;

    while (pending < pendingGPUFrames.size())
    {
        Frame& frame = frames[pendingGPUFrames[pending]];

        const RHIGPUTimingStatus status = frame.gpuTiming->GetStatus();

        if (status != RHIGPUTimingStatus::ePending || abandon)
        {
            frame.gpuStatus = status;

            if (status == RHIGPUTimingStatus::ePending)
            {
                frame.gpuStatus = RHIGPUTimingStatus::eDropped;

                ++droppedGPUFrames;
            }
            else
            {
                frame.gpuUs = frame.gpuTiming->GetMicroseconds();

                frame.gpuIntervals = frame.gpuTiming->GetIntervalCount();

                frame.gpuExcludedIntervals = frame.gpuTiming->GetExcludedIntervalCount();
            }

            frame.gpuTiming.Reset();

            pendingGPUFrames.erase(pendingGPUFrames.begin() + pending);
        }
        else
        {
            ++pending;
        }
    }
}

void SceneRendererProfiling::State::RetainCurrentFrame()
{
    CollectGPUFrames();

    if (frames.size() < MaxProfileFrames)
    {
        if (pendingGPUFrames.size() == MaxPendingGPUFrames)
        {
            Frame& oldest = frames[pendingGPUFrames.front()];

            // Drop only the export ownership; the native recording retains its result.
            oldest.gpuStatus = RHIGPUTimingStatus::eDropped;

            oldest.gpuTiming.Reset();

            pendingGPUFrames.erase(pendingGPUFrames.begin());

            ++droppedGPUFrames;
        }

        pendingGPUFrames.push_back(frames.size());

        frames.push_back(current);
    }
    else
    {
        ++droppedFrames;
    }

    current.gpuTiming.Reset();

    CollectGPUFrames();
}

void SceneRendererProfiling::State::WriteFramesCSV(std::ostream& output,
                                                   ProfileFrameSummary& summary) const
{
    output
        << "run_id,frame_index,phase,phase_frame,cpu_frame_ms,succeeded,width,height,requested_mode,resolved_mode,gi_method,gpu_status,gpu_frame_ms,gpu_intervals,gpu_excluded_intervals,frame_start_ms,frame_end_ms,rhi_execution_ms,draws,dispatches,submissions,descriptor_hits,descriptor_misses,descriptor_inserts,descriptor_retirements,bindless_captures\n";

    for (const State::Frame& frame : frames)
    {
        output << runID << ',' << frame.index << ',' << frame.phase << ',' << frame.local << ','
               << frame.cpuUs / 1000 << ',' << frame.succeeded << ',' << frame.width << ','
               << frame.height << ',' << frame.requestedMode << ',' << frame.resolvedMode << ',';

        CSVString(output, frame.method);

        output << ',' << RHIGPUTimingStatusName(frame.gpuStatus) << ',';

        if (frame.gpuStatus == RHIGPUTimingStatus::eAvailable)
        {
            output << frame.gpuUs / 1000;

            if (frame.succeeded)
            {
                summary.gpu[frame.phase].push_back(frame.gpuUs / 1000);
            }
        }
        else
        {
            ++summary.unavailable[frame.phase];
        }

        output << ',' << frame.gpuIntervals << ',' << frame.gpuExcludedIntervals << ','
               << frame.startUs / 1000 << ',' << frame.endUs / 1000 << ','
               << frame.rhiCPUUs / 1000.0 << ',' << frame.counters.draws << ','
               << frame.counters.dispatches << ',' << frame.counters.submissions << ','
               << frame.counters.descriptorHits << ',' << frame.counters.descriptorMisses << ','
               << frame.counters.descriptorInserts << ',' << frame.counters.descriptorRetirements
               << ',' << frame.counters.bindlessCaptures;

        output << '\n';

        if (frame.succeeded)
        {
            summary.cpu[frame.phase].push_back(frame.cpuUs / 1000);
        }
    }
}

void SceneRendererProfiling::State::WritePassesCSV(std::ostream& output,
                                                   ProfilePassSummaries& summaries) const
{
    output
        << "run_id,graph_record,frame_index,phase,graph,execution,transfer_only,node_id,node_order,pass,node_type,queue,queue_equivalence,submission_group,cpu_record_us,gpu_status,gpu_us\n";

    for (const State::Pass& pass : passes)
    {
        const State::Graph& graph = graphs[pass.graph];

        const char* phase = Phase(graph.frame);

        const rc::RDGNodeMetrics& node = pass.node;

        output << runID << ',' << pass.graph << ',';

        if (graph.frame != UINT64_MAX)
        {
            output << graph.frame;
        }

        output << ',' << phase << ',';

        CSVString(output, graph.name.CStr());

        output << ',' << graph.execution << ',' << graph.transfer << ',' << node.id << ','
               << node.order << ',';

        CSVString(output, node.name.CStr());

        output << ',' << static_cast<uint32_t>(node.type) << ',' << QueueName(node.plannedQueue)
               << ',';

        if (pass.queueEquivalence != UINT32_MAX)
        {
            output << pass.queueEquivalence;
        }

        output << ',';

        if (node.submissionGroup != UINT32_MAX)
        {
            output << node.submissionGroup;
        }

        output << ',';

        if (pass.cpuMeasured)
        {
            output << node.recordCPUUs;
        }

        output << ',' << RHIGPUTimingStatusName(node.gpuStatus) << ',';

        ProfilePassSummary& summary =
            summaries[{phase, graph.name.CStr(), node.name.CStr(),
                       static_cast<uint32_t>(node.plannedQueue), pass.queueEquivalence}];

        if (node.gpuStatus == RHIGPUTimingStatus::eAvailable)
        {
            output << node.gpuUs;

            summary.gpu.push_back(node.gpuUs);
        }
        else
        {
            ++summary.unavailable;
        }

        if (pass.cpuMeasured)
        {
            summary.cpu.push_back(node.recordCPUUs);
        }

        output << '\n';
    }
}

void SceneRendererProfiling::State::WriteSummaryJSON(std::ostream& output,
                                                     ProfileFrameSummary& frameSummary,
                                                     ProfilePassSummaries& summaries,
                                                     bool runSucceeded,
                                                     bool unchanged) const
{
    output << "{\"schema_version\":4,\"run_id\":";

    JSONString(output, runID);

    output << ",\"run_succeeded\":" << runSucceeded << ",\"input_files_unchanged\":" << unchanged
           << ",\"instrumented\":true,\"inputs\":" << inputIdentity
           << ",\"settings\":" << (settings.empty() ? "null" : settings) << ",\"command\":[";

    for (size_t i = 0; i < options.arguments.size(); ++i)
    {
        output << (i == 0 ? "" : ",");

        JSONString(output, options.arguments[i]);
    }

    output << "],\"device\":{\"name\":";

    JSONString(output, gpu.deviceName.data());

    output << ",\"vendor_id\":" << gpu.vendorID << ",\"device_id\":" << gpu.deviceID
           << ",\"api_version_raw\":" << gpu.apiVersion
           << ",\"driver_version_raw\":" << gpu.driverVersionRaw << "}"
           << ",\"build\":{\"compiler\":";
#if defined(_MSC_VER)
    JSONString(output, "MSVC " + std::to_string(_MSC_VER));
#elif defined(__clang__)
    JSONString(output, "Clang " __clang_version__);
#else
    JSONString(output, "GCC " __VERSION__);
#endif
#if defined(NDEBUG)
    output << ",\"ndebug\":true";
#else
    output << ",\"ndebug\":false";
#endif
    output
        << "},\"requested_frames\":" << options.frames
        << ",\"requested_warmup_frames\":" << options.warmup
        << ",\"gi_start_frame\":" << options.giStartFrame << ",\"fixed_step\":" << options.fixedStep
        << ",\"fixed_step_scope\":\"light and motion animation\""
        << ",\"phase_policy\":\"startup has no application frame; cold is the first rendered frame or explicit GI activation; warmup is excluded; measured is the requested measurement interval\""
        << ",\"gpu_scope\":\"per-pass elapsed intervals including barriers; pass durations are not summed\""
        << ",\"gpu_frame_scope\":\"elapsed earliest TOP to latest BOTTOM across native graphics/compute-capable command buffers begun during the application render workload, including graph preparation GPU work and the graphics swapchain copy; excludes dedicated transfer-only buffers, asset startup outside the frame, and screenshot readback; does not directly measure host acquire/present calls or compositor/display latency; dependency waits and submission gaps may be included; requires a common device timestamp domain; this is not GPU-active time or occupancy\""
        << ",\"cpu_scope\":\"application frame wall time includes submission and frame-slot backpressure; pass CPU records measure command recording\""
        << ",\"memory_scope\":\"RDG transient allocation accounting, not hardware residency\""
        << ",\"percentile_policy\":\"median averages the two central samples; p95 uses nearest rank\""
        << ",\"limits\":{\"frames\":" << MaxProfileFrames << ",\"graphs\":" << MaxProfileGraphs
        << ",\"passes\":" << MaxProfilePasses << ",\"pending_gpu_frames\":" << MaxPendingGPUFrames
        << "},\"dropped\":{\"frames\":" << droppedFrames << ",\"graphs\":" << droppedGraphs
        << ",\"passes\":" << droppedPasses << ",\"gpu_frames\":" << droppedGPUFrames
        << "},\"frame_count\":" << frames.size() << ",\"pass_count\":" << passes.size()
        << ",\"cpu_frame_ms\":{";

    bool separator = false;

    for (const char* phase : {"cold", "warmup", "measured"})
    {
        output << (separator ? "," : "");

        JSONString(output, phase);

        output << ':';

        WriteStatistics(output, frameSummary.cpu[phase]);

        separator = true;
    }

    output << "},\"gpu_frame_ms\":{";

    separator = false;

    for (const char* phase : {"cold", "warmup", "measured"})
    {
        output << (separator ? "," : "");

        JSONString(output, phase);

        output << ':';

        WriteStatistics(output, frameSummary.gpu[phase]);

        separator = true;
    }

    output << "},\"gpu_frame_unavailable\":{";

    separator = false;

    for (const char* phase : {"cold", "warmup", "measured"})
    {
        output << (separator ? "," : "");

        JSONString(output, phase);

        output << ':' << frameSummary.unavailable[phase];

        separator = true;
    }

    output << "},\"graphs\":[";

    for (size_t i = 0; i < graphs.size(); ++i)
    {
        const State::Graph& graph = graphs[i];

        output << (i == 0 ? "" : ",") << "{\"record\":" << i << ",\"graph\":";

        JSONString(output, graph.name.CStr());

        output << ",\"execution\":" << graph.execution << ",\"frame_index\":";

        if (graph.frame == UINT64_MAX)
        {
            output << "null";
        }
        else
        {
            output << graph.frame;
        }

        output << ",\"phase\":";

        JSONString(output, Phase(graph.frame));

        output << ",\"transfer_only\":" << graph.transfer
               << ",\"compile_cpu_us\":" << graph.compileUs
               << ",\"execute_cpu_us\":" << graph.executeUs
               << ",\"submission_cpu_us\":" << graph.submissionUs
               << ",\"assigned_transient_bytes\":" << graph.assignedBytes
               << ",\"available_transient_bytes\":" << graph.availableBytes
               << ",\"retiring_transient_bytes\":" << graph.retiringBytes
               << ",\"node_count\":" << graph.nodeCount
               << ",\"omitted_nodes\":" << graph.omittedNodes
               << ",\"omitted_submissions\":" << graph.omittedSubmissions << '}';
    }

    output << "],\"pass_statistics\":[";

    separator = false;

    for (ProfilePassSummaries::value_type& entry : summaries)
    {
        const ProfileSummaryKey& key = entry.first;

        ProfilePassSummary& summary = entry.second;

        output << (separator ? "," : "") << "{\"phase\":";

        JSONString(output, std::get<0>(key));

        output << ",\"graph\":";

        JSONString(output, std::get<1>(key));

        output << ",\"pass\":";

        JSONString(output, std::get<2>(key));

        output << ",\"queue\":";

        JSONString(output, QueueName(static_cast<RHICommandContextType>(std::get<3>(key))));

        output << ",\"queue_equivalence\":";

        if (std::get<4>(key) == UINT32_MAX)
        {
            output << "null";
        }
        else
        {
            output << std::get<4>(key);
        }

        output << ",\"gpu_unavailable\":" << summary.unavailable << ",\"cpu_record_us\":";

        WriteStatistics(output, summary.cpu);

        output << ",\"gpu_us\":";

        WriteStatistics(output, summary.gpu);

        output << '}';

        separator = true;
    }

    output << "]}\n";
}

SceneRendererProfiling::SceneRendererProfiling(rc::RenderDevice& device,
                                               const DemoProfilingOptions& options) :
    m_state(MakeUnique<State>())
{
    State& state = *m_state;

    state.options = options;

    state.gpu = device.GetGPUInfo();

    state.queues = device.GetQueueCapabilities();

    state.runID = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());

    state.inputIdentity = FingerprintInputs(options);

    std::ifstream config(ZEN_CONFIG_PATH, std::ios::binary);

    state.configuration.assign(std::istreambuf_iterator<char>(config),
                               std::istreambuf_iterator<char>());

    rc::RDGMetrics& metrics = device.GetRDGMetrics();

    rc::RDGMetricsOptions settings = metrics.GetOptions();

    settings.logging.enabled = true;

    settings.logging.sampleEvery = 1;

    settings.logging.minInterval = std::chrono::milliseconds::zero();

    settings.nodeTimings = true;

    settings.preparationTimings = true;

    settings.gpuTimings = true;

    settings.includeTransferNodes = true;

    settings.maxNodeDetails = 4096;

    settings.maxSubmissionDetails = 4096;

    settings.maxDependencyDetails = 4096;

    settings.maxPendingGPUCaptures = 32;

    metrics.Configure(settings);

    metrics.SetFrameIndex(UINT64_MAX);

    metrics.SetSink({});

    metrics.SetGPUSink(
        [this](const rc::RDGMetricsSnapshot& snapshot) { m_state->Capture(snapshot); });
}

SceneRendererProfiling::~SceneRendererProfiling() = default;

void SceneRendererProfiling::BeginFrame(rc::RenderDevice& device, uint32_t localFrame, bool warmup)
{
    if (m_state->active)
    {
        State& state = *m_state;

        if (state.frameOpen)
        {
            GDynamicRHI->EndGPUFrameTiming(state.current.gpuTiming, false);

            state.RetainCurrentFrame();
        }

        state.CollectGPUFrames();

        state.current = {};

        const RHIThreadMetrics metrics = device.GetRHIThreadMetrics();

        state.current.rhiStartUs = metrics.executionCPUUs;

        state.current.counterStart = metrics.native;

        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        if (state.frames.empty())
        {
            state.frameOrigin = now;
        }

        state.current.startUs =
            std::chrono::duration<double, std::micro>(now - state.frameOrigin).count();

        state.current.index = rc::ToValue(GRenderFrameState.GetFrameNumber());

        state.current.local = localFrame;

        state.current.phase = warmup ? "warmup" : "measured";

        if (!warmup && state.options.giStartFrame != 0)
        {
            state.current.phase = localFrame < state.options.giStartFrame ? "warmup" :
                localFrame == state.options.giStartFrame                  ? "cold" :
                                                                            "measured";
        }
        else if (state.frames.empty() && state.droppedFrames == 0)
        {
            state.current.phase = "cold";
        }

        device.GetRDGMetrics().SetFrameIndex(state.current.index);

        state.current.gpuTiming = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        state.frameOpen = true;

        GDynamicRHI->BeginGPUFrameTiming(state.current.gpuTiming);
    }
}

void SceneRendererProfiling::RecordFrame(rc::RenderDevice& device,
                                         RHIViewport& viewport,
                                         double cpuUs,
                                         bool succeeded)
{
    if (m_state->active && m_state->frameOpen)
    {
        State& state = *m_state;

        rc::RendererServer& server = *device.GetRendererServer();

        State::Frame& frame = state.current;

        frame.cpuUs = cpuUs;

        frame.endUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                                state.frameOrigin)
                          .count();

        const RHIThreadMetrics metrics = device.GetRHIThreadMetrics();

        frame.rhiCPUUs = metrics.executionCPUUs - frame.rhiStartUs;

        frame.counters.draws = metrics.native.draws - frame.counterStart.draws;

        frame.counters.dispatches = metrics.native.dispatches - frame.counterStart.dispatches;

        frame.counters.submissions = metrics.native.submissions - frame.counterStart.submissions;

        frame.counters.descriptorHits =
            metrics.native.descriptorHits - frame.counterStart.descriptorHits;

        frame.counters.descriptorMisses =
            metrics.native.descriptorMisses - frame.counterStart.descriptorMisses;

        frame.counters.descriptorInserts =
            metrics.native.descriptorInserts - frame.counterStart.descriptorInserts;

        frame.counters.descriptorRetirements =
            metrics.native.descriptorRetirements - frame.counterStart.descriptorRetirements;

        frame.counters.bindlessCaptures =
            metrics.native.bindlessCaptures - frame.counterStart.bindlessCaptures;

        frame.succeeded = succeeded && !device.AreSubmissionsBlocked();

        GDynamicRHI->EndGPUFrameTiming(frame.gpuTiming, frame.succeeded);

        state.frameOpen = false;

        frame.width = viewport.GetWidth();

        frame.height = viewport.GetHeight();

        frame.requestedMode = static_cast<uint32_t>(server.GetRequestedRenderOption()) + 1;

        frame.resolvedMode = static_cast<uint32_t>(server.GetRenderOption()) + 1;

        frame.method = server.GetRenderOption() == rc::RenderOption::eVoxelGI ? "cone" : "none";

        state.RetainCurrentFrame();

        device.GetRDGMetrics().CollectGPUResults();
    }
}

void SceneRendererProfiling::Stop(rc::RenderDevice& device,
                                  const rc::RenderScene* scene,
                                  RHIViewport& viewport)
{
    if (m_state->active)
    {
        State& state = *m_state;

        if (state.frameOpen)
        {
            RecordFrame(device, viewport, 0, false);
        }

        state.active = false;

        rc::RDGMetricsOptions metrics = device.GetRDGMetrics().GetOptions();

        metrics.logging.enabled = false;

        metrics.gpuTimings = false;

        device.GetRDGMetrics().Configure(metrics);

        const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();

        const rc::RendererServer& server = *device.GetRendererServer();

        const rc::VoxelizerBase& voxels = *server.RequestVoxelizer();

        const rc::VoxelGISettings& cone = server.RequestVoxelGI()->GetSettings();

        std::ostringstream output;

        ConfigureStream(output);

        output << "{\"viewport_width\":" << viewport.GetWidth()
               << ",\"viewport_height\":" << viewport.GetHeight()
               << ",\"gbuffer_size\":" << rc::RenderConfig::GetInstance().offScreenFbSize
               << ",\"frames_in_flight\":" << rc::RenderConfig::GetInstance().numFrames
               << ",\"rhi_threaded\":" << GetRHIThread().IsThreaded()
               << ",\"async_compute_status\":";

        JSONString(output, rc::GetAsyncComputeStatusReason(device.GetAsyncComputeStatus()));

        output
            << ",\"vsync_requested\":" << state.options.vsync
            << ",\"vsync_note\":\"VSync chooses the existing platform present-mode policy; compositor behavior is not measured\""
            << ",\"validation_enabled\":" << RHIOptions::GetInstance().ValidationEnabled()
            << ",\"ray_tracing_enabled\":" << RHIOptions::GetInstance().RayTracingEnabled()
            << ",\"gpu_markers\":" << RHIOptions::GetInstance().GPUProfilerMarkers()
            << ",\"voxelizer\":";

        JSONString(output,
                   rc::ResolveVoxelizerMode(config.GetVoxelizerMode(), device.GetGPUInfo()) ==
                           platform::VoxelizerMode::eGeometry ?
                       "geom" :
                       "comp");

        output << ",\"voxel_resolution\":" << voxels.GetVoxelTexResolution()
               << ",\"reflectance_policy\":";

        JSONString(output, voxels.UsesAveragedReflectance() ? "averaged" : "owner");

        output << ",\"gi_method\":\"cone\""
               << ",\"analytic_lighting\":" << cone.analyticLighting
               << ",\"environment_lighting\":" << cone.environmentLighting
               << ",\"emissive_lighting\":" << cone.emissiveLighting
               << ",\"indirect_intensity\":" << cone.indirectIntensity
               << ",\"shadows\":" << cone.shadows << ",\"cone_count\":" << cone.coneCount
               << ",\"cone_max_steps\":" << cone.maxSteps
               << ",\"cone_angle_degrees\":" << cone.coneAngleDegrees
               << ",\"cone_step_scale\":" << cone.stepScale
               << ",\"cone_normal_bias_voxels\":" << cone.normalBiasVoxels
               << ",\"cone_max_distance_grid_lengths\":" << cone.maxDistanceGridLengths;

        if (scene != nullptr)
        {
            const rc::SceneUniformData& data =
                *reinterpret_cast<const rc::SceneUniformData*>(scene->GetSceneUniformData());

            output << ",\"scene_geometry_revision\":" << scene->GetGeometryRevision()
                   << ",\"scene_surface_revision\":" << scene->GetSurfaceRevision()
                   << ",\"final_camera_position\":";

            WriteVec4(output, Vec4(data.viewPos));

            output << ",\"environment_intensity_rotation_enabled_visible\":";

            WriteVec4(output, data.environment);

            output << ",\"final_lights\":[";

            for (uint32_t i = 0; i < static_cast<uint32_t>(data.lightInfo.x); ++i)
            {
                const rc::GPULight& light = data.lights[i];

                output << (i == 0 ? "" : ",") << "{\"position_range\":";

                WriteVec4(output, light.positionRange);

                output << ",\"direction_type\":";

                WriteVec4(output, light.directionType);

                output << ",\"color_intensity\":";

                WriteVec4(output, light.colorIntensity);

                output << ",\"cone_shadow\":";

                WriteVec4(output, light.coneShadow);

                output << '}';
            }

            output << ']';
        }

        output << '}';

        state.settings = output.str();
    }
}

bool SceneRendererProfiling::Export(rc::RDGMetrics& metrics, bool runSucceeded)
{
    metrics.CollectGPUResults(true);

    metrics.SetGPUSink({});

    State& state = *m_state;

    state.CollectGPUFrames(true);

    const bool unchanged = state.inputIdentity == FingerprintInputs(state.options);

    const std::filesystem::path prefix(state.options.prefix);

    std::error_code error;

    if (!prefix.parent_path().empty())
    {
        std::filesystem::create_directories(prefix.parent_path(), error);
    }

    std::ofstream frames(prefix.string() + ".frames.csv"), passes(prefix.string() + ".passes.csv");

    std::ofstream config(prefix.string() + ".config.cfg", std::ios::binary);

    ConfigureStream(frames);

    ConfigureStream(passes);

    config.write(state.configuration.data(),
                 static_cast<std::streamsize>(state.configuration.size()));

    ProfileFrameSummary frameSummary;

    state.WriteFramesCSV(frames, frameSummary);

    ProfilePassSummaries summaries;

    state.WritePassesCSV(passes, summaries);

    frames.flush();

    passes.flush();

    config.flush();

    bool valid = !error && frames.good() && passes.good() && config.good();

    std::ofstream output(prefix.string() + ".profile.json");

    ConfigureStream(output);

    state.WriteSummaryJSON(output, frameSummary, summaries, runSucceeded, unchanged);

    output.flush();

    valid = valid && output.good() && unchanged;

    if (state.droppedFrames || state.droppedGraphs || state.droppedPasses || state.droppedGPUFrames)
    {
        LOGW("Profile capture limit reached: omitted frames={} graphs={} passes={} gpu_frames={}",
             state.droppedFrames, state.droppedGraphs, state.droppedPasses, state.droppedGPUFrames);
    }

    if (!valid)
    {
        LOGE("Profile export failed or input files changed during capture: {}",
             state.options.prefix);
    }
    else
    {
        LOGI("Profile exported: {} ({} frames, {} passes)", state.options.prefix,
             state.frames.size(), state.passes.size());
    }

    return valid;
}
} // namespace zen
