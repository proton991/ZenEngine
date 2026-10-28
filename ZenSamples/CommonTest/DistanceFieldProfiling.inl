// Optional construction-only timestamps. This helper owns the dedicated test
// device's metrics sinks; the production field owner never waits for readback.
class FieldAssetGPUProfile
{
public:
    ~FieldAssetGPUProfile()
    {
        Close();
    }

    bool Init(RenderDevice& device, const char* prefix)
    {
        bool valid = true;
        if (prefix != nullptr)
        {
            m_passes = std::fopen((std::string(prefix) + "-passes.csv").c_str(), "w");
            m_frames = std::fopen((std::string(prefix) + "-frames.csv").c_str(), "w");
            valid    = m_passes != nullptr && m_frames != nullptr;
            if (valid)
            {
                m_device                      = &device;
                m_saved                       = device.GetRDGMetrics().GetOptions();
                RDGMetricsOptions options     = m_saved;
                options.logging.enabled       = true;
                options.logging.sampleEvery   = 1;
                options.logging.minInterval   = std::chrono::milliseconds::zero();
                options.nodeTimings           = true;
                options.gpuTimings            = true;
                options.includeTransferNodes  = true;
                options.maxNodeDetails        = 1024;
                options.maxPendingGPUCaptures = 32;
                device.GetRDGMetrics().Configure(options);
                device.GetRDGMetrics().SetSink({});
                device.GetRDGMetrics().SetGPUSink([this](const RDGMetricsSnapshot& snapshot) { Capture(snapshot); });
                std::fprintf(m_passes, "frame,graph,execution,order,pass,queue,status,gpu_us\n");
                std::fprintf(m_frames, "frame,status,intervals,excluded_intervals,gpu_us\n");
            }
        }
        return valid;
    }

    void Begin(uint32_t frame)
    {
        if (m_device != nullptr)
        {
            m_device->GetRDGMetrics().SetFrameIndex(frame);
            m_current = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();
            m_frame   = frame;
            GDynamicRHI->BeginGPUFrameTiming(m_current);
        }
    }

    void End(bool submitted)
    {
        if (m_device != nullptr)
        {
            GDynamicRHI->EndGPUFrameTiming(m_current, submitted);
            m_pending.push_back({m_frame, m_current});
            m_current.Reset();
        }
    }

    void Collect(bool abandon = false)
    {
        if (m_device != nullptr)
        {
            m_device->GetRDGMetrics().CollectGPUResults(abandon);
            for (size_t index = 0; index < m_pending.size();)
            {
                const Pending&     pending = m_pending[index];
                RHIGPUTimingStatus status  = pending.timing->GetStatus();
                if (status != RHIGPUTimingStatus::ePending || abandon)
                {
                    status = status == RHIGPUTimingStatus::ePending ? RHIGPUTimingStatus::eDropped : status;
                    const double microseconds =
                        status == RHIGPUTimingStatus::eAvailable ? pending.timing->GetMicroseconds() : 0;
                    std::fprintf(m_frames, "%u,%s,%zu,%zu,%.6f\n", pending.frame, RHIGPUTimingStatusName(status),
                                 pending.timing->GetIntervalCount(), pending.timing->GetExcludedIntervalCount(), microseconds);
                    if (status == RHIGPUTimingStatus::eAvailable)
                    {
                        ++availableFrames;
                        maximumFrameUs = std::max(maximumFrameUs, microseconds);
                    }
                    else if (pending.timing->GetIntervalCount() != 0 || pending.timing->GetExcludedIntervalCount() != 0)
                    {
                        ++unavailableFrames;
                    }
                    m_pending.erase(m_pending.begin() + index);
                }
                else
                {
                    ++index;
                }
            }
        }
    }

    void Finish()
    {
        if (m_device != nullptr)
        {
            // Test capture teardown, outside the measured preparation loop.
            m_device->FlushRHIThread();
            m_device->WaitForIdle();
            m_device->CollectCompletedResources();
            m_device->FlushRHIThread();
            Collect(true);
        }
        Close();
    }

    uint64_t availableFrames{0};
    uint64_t unavailableFrames{0};
    uint64_t availablePasses{0};
    uint64_t unavailablePasses{0};
    double   maximumFrameUs{0};
    double   maximumPassUs{0};

private:
    struct Pending
    {
        uint32_t             frame;
        RHIGPUFrameTimingPtr timing;
    };

    void Capture(const RDGMetricsSnapshot& snapshot)
    {
        for (const RDGNodeMetrics& node : snapshot.nodes)
        {
            std::fprintf(m_passes, "%llu,%s,%llu,%u,%s,%u,%s,%.6f\n", static_cast<unsigned long long>(snapshot.frameIndex),
                         snapshot.graph.CStr(), static_cast<unsigned long long>(snapshot.execution), node.order,
                         node.name.CStr(), static_cast<unsigned int>(node.plannedQueue), RHIGPUTimingStatusName(node.gpuStatus),
                         node.gpuUs);
            if (node.gpuStatus == RHIGPUTimingStatus::eAvailable)
            {
                ++availablePasses;
                maximumPassUs = std::max(maximumPassUs, node.gpuUs);
            }
            else
            {
                ++unavailablePasses;
            }
        }
        unavailablePasses += snapshot.omittedNodes;
    }

    void Close()
    {
        if (m_device != nullptr)
        {
            m_device->GetRDGMetrics().SetGPUSink({});
            m_device->GetRDGMetrics().Configure(m_saved);
            m_device = nullptr;
        }
        if (m_passes != nullptr)
        {
            std::fclose(m_passes);
            m_passes = nullptr;
        }
        if (m_frames != nullptr)
        {
            std::fclose(m_frames);
            m_frames = nullptr;
        }
    }

    RenderDevice*        m_device{nullptr};
    RDGMetricsOptions    m_saved;
    FILE*                m_passes{nullptr};
    FILE*                m_frames{nullptr};
    RHIGPUFrameTimingPtr m_current;
    HeapVector<Pending>  m_pending;
    uint32_t             m_frame{0};
};
