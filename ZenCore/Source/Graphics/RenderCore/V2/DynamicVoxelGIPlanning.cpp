#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/Shared/VoxelGI.h"
#include "Graphics/Shared/DynamicVoxelGI.h"
#include <limits>

namespace zen::rc
{
bool LoadDynamicVoxelGISettings(const platform::ConfigLoader& config,
                                DynamicVoxelGISettings& output)
{
    DynamicVoxelGISettings settings;
    settings.resolution        = config.GetVoxelResolution();
    const std::string method  = config.GetString("voxel_gi_method", "auto");
    const std::string backend = config.GetString("dynamic_voxel_gi_query_backend", "auto");
    const std::string temporal = config.GetString("dynamic_voxel_gi_temporal_filter", "elapsed");
    const std::string spatial  = config.GetString("dynamic_voxel_gi_spatial_filter", "true");
    const std::string cache    = config.GetString("dynamic_voxel_gi_cache", "compact");
    bool valid                = method == "auto" || method == "cone" || method == "dynamic_voxel";
    valid = valid && (backend == "auto" || backend == "voxel_dda" || backend == "hardware_rt");
    valid = valid && (temporal == "off" || temporal == "fixed" || temporal == "elapsed") &&
        (spatial == "true" || spatial == "false");
    settings.temporal      = temporal == "off" ? GITemporalMode::eOff :
        temporal == "fixed"                    ? GITemporalMode::eFixed :
                                                 GITemporalMode::eElapsed;
    settings.spatialFilter = spatial == "true";
    valid                  = valid && (cache == "decoded" || cache == "compact");
    settings.compactCache  = cache == "compact";
    valid                  = valid &&
        config.ReadBool("dynamic_voxel_gi_analytic_lighting", settings.analyticLighting) &&
        config.ReadBool("dynamic_voxel_gi_environment_lighting", settings.environmentLighting) &&
        config.ReadBool("dynamic_voxel_gi_emissive_lighting", settings.emissiveLighting);
    settings.method    = method == "dynamic_voxel" ?
        VoxelGIMethod::eDynamicVoxel :
        (method == "cone" ? VoxelGIMethod::eCone : VoxelGIMethod::eAuto);
    settings.backend   = backend == "hardware_rt" ?
        VoxelGIQueryBackend::eHardwareRT :
        (backend == "voxel_dda" ? VoxelGIQueryBackend::eVoxelDDA : VoxelGIQueryBackend::eAuto);
    uint64_t budgetMiB = 0;
    valid              = valid && config.ReadNumber("voxel_resolution", settings.resolution) &&
        config.ReadNumber("dynamic_voxel_gi_rays_per_face", settings.raysPerFace) &&
        config.ReadNumber("dynamic_voxel_gi_neighbor_radius", settings.neighborRadius) &&
        config.ReadNumber("dynamic_voxel_gi_memory_budget_mb", budgetMiB);
    valid = valid &&
        (settings.resolution == 64 || settings.resolution == 128 || settings.resolution == 256) &&
        (settings.raysPerFace == 32 || settings.raysPerFace == 64 || settings.raysPerFace == 128) &&
        (settings.neighborRadius == 1 || settings.neighborRadius == 2) &&
        budgetMiB <= std::numeric_limits<uint64_t>::max() / (1024 * 1024) &&
        (!config.HasKey("dynamic_voxel_gi_memory_budget_mb") || budgetMiB > 0);
    if (valid)
    {
        settings.memoryBudgetBytes = budgetMiB * 1024 * 1024;
        output                     = settings;
    }
    else
    {
        LOGE("Invalid dynamic voxel GI configuration; settings were not applied");
    }
    return valid;
}

VoxelGISelection ResolveVoxelGISelection(const DynamicVoxelGISettings& settings)
{
    VoxelGISelection selection;
    if (settings.method == VoxelGIMethod::eCone)
    {
        selection.reason = "explicit cone method; query backend inactive";
    }
    else if (settings.method == VoxelGIMethod::eDynamicVoxel ||
             settings.backend != VoxelGIQueryBackend::eAuto)
    {
        selection.reason = settings.backend == VoxelGIQueryBackend::eHardwareRT ?
            "hardware RT provider and dynamic voxel GI unavailable; using cone" :
            "dynamic voxel GI awaits scene-profile and resource checks; using cone";
    }
    return selection;
}

GIResourceStatus ValidateGIStorageBuffer(uint64_t elements,
                                         uint32_t stride,
                                         const RHIGPUInfo& gpu,
                                         uint64_t& bytes)
{
    GIResourceStatus status = GIResourceStatus::eSuccess;
    if (stride == 0)
    {
        status = GIResourceStatus::eInvalidInput;
    }
    else if (elements > std::numeric_limits<uint64_t>::max() / stride)
    {
        status = GIResourceStatus::eOverflow;
    }
    else
    {
        bytes = elements * stride;
        if (bytes > std::numeric_limits<uint32_t>::max())
        {
            status = GIResourceStatus::eBufferSize;
        }
        else if (bytes > gpu.maxStorageBufferRange)
        {
            status = GIResourceStatus::eDescriptorRange;
        }
    }
    return status;
}

GIResourceStatus ValidateVoxelReflectanceResources(uint32_t resolution,
                                                   uint64_t triangles,
                                                   uint64_t budgetBytes,
                                                   uint64_t retiringBytes,
                                                   const RHIGPUInfo& gpu,
                                                   uint64_t& peakBytes)
{
    GIResourceStatus status = GIResourceStatus::eInvalidInput;
    peakBytes               = 0;
    if ((resolution == 64 || resolution == 128 || resolution == 256) && budgetBytes != 0)
    {
        const uint64_t cells  = uint64_t(resolution) * resolution * resolution;
        uint64_t scratchBytes = 0;
        status                = ValidateGIStorageBuffer(cells, 16, gpu, scratchBytes);
        if (status == GIResourceStatus::eSuccess)
        {
            const uint64_t currentBytes = scratchBytes + cells * 4;
            if (triangles > std::numeric_limits<uint32_t>::max() / ZEN_VOXEL_REFLECTANCE_SCALE ||
                retiringBytes > std::numeric_limits<uint64_t>::max() - currentBytes)
            {
                status = GIResourceStatus::eOverflow;
            }
            else
            {
                peakBytes = currentBytes + retiringBytes;
                status    = peakBytes <= budgetBytes ? GIResourceStatus::eSuccess :
                                                       GIResourceStatus::eBudget;
            }
        }
    }
    return status;
}

GIResourceStatus ValidateVoxelConeResources(uint32_t resolution,
                                            bool averaged,
                                            uint64_t budgetBytes,
                                            uint64_t retiringBytes,
                                            const RHIGPUInfo& gpu,
                                            uint64_t& peakBytes)
{
    GIResourceStatus status = GIResourceStatus::eInvalidInput;
    peakBytes               = 0;
    if ((resolution == 64 || resolution == 128 || resolution == 256) && budgetBytes != 0)
    {
        const uint64_t cells = uint64_t(resolution) * resolution * resolution;
        uint64_t bytes       = 0;
        status =
            averaged ? ValidateGIStorageBuffer(cells, 16, gpu, bytes) : GIResourceStatus::eSuccess;
        uint64_t mipCells    = 0;
        for (uint32_t n = resolution; n != 0; n /= 2)
        {
            mipCells += uint64_t(n) * n * n;
        }
        // Owner/normal/emission/sky, albedo/radiance mips and optional V1.
        const uint64_t current = cells * (averaged ? 44 : 24) + mipCells * 12;
        if (status == GIResourceStatus::eSuccess)
        {
            if (retiringBytes > std::numeric_limits<uint64_t>::max() - current)
            {
                status = GIResourceStatus::eOverflow;
            }
            else
            {
                peakBytes = current + retiringBytes;
                if (peakBytes > budgetBytes)
                {
                    status = GIResourceStatus::eBudget;
                }
            }
        }
    }
    return status;
}

GIResourceStatus ValidateVoxelClassResources(uint32_t resolution,
                                             bool averaged,
                                             uint64_t budgetBytes,
                                             uint64_t retiringBytes,
                                             const RHIGPUInfo& gpu,
                                             uint64_t& peakBytes)
{
    GIResourceStatus status = ValidateVoxelConeResources(resolution, averaged, budgetBytes,
                                                         retiringBytes, gpu, peakBytes);
    if (status == GIResourceStatus::eSuccess)
    {
        uint64_t mapBytes = 0;
        status = ValidateGIStorageBuffer(uint64_t(resolution) * resolution * resolution, 4, gpu,
                                         mapBytes);
        // Two class surface sets, list/map pairs, independent V1 scratch and counts.
        const uint64_t classes =
            uint64_t(resolution) * resolution * resolution * (averaged ? 96 : 56) + 8;
        if (classes > UINT64_MAX - peakBytes)
        {
            status = GIResourceStatus::eOverflow;
        }
        else if (status == GIResourceStatus::eSuccess)
        {
            peakBytes += classes;
            status =
                peakBytes <= budgetBytes ? GIResourceStatus::eSuccess : GIResourceStatus::eBudget;
        }
    }
    return status;
}

GIResourceStatus PlanStaticVoxelGIResources(uint32_t resolution,
                                            bool averaged,
                                            uint64_t budgetBytes,
                                            const RHIGPUInfo& gpu,
                                            uint32_t& capacity,
                                            uint64_t& peakBytes,
                                            uint64_t receiverSurfaceBytes,
                                            uint64_t retiringBytes,
                                            bool compactCache,
                                            uint32_t raysPerFace)
{
    capacity = 0;
    GIResourceStatus status = ValidateVoxelClassResources(resolution, averaged, budgetBytes,
                                                          retiringBytes, gpu, peakBytes);
    const uint64_t cells =
        status == GIResourceStatus::eSuccess ? uint64_t(resolution) * resolution * resolution : 0;
    uint64_t argumentBytes = 0;
    if (status == GIResourceStatus::eSuccess)
    {
        ComputeDispatchChunk gridChunk, receiverChunk;
        const bool dispatchValid =
            BuildComputeDispatchChunk(0, static_cast<uint32_t>(cells), GI_QUERY_GROUP_SIZE, gpu, gridChunk) &&
            BuildComputeDispatchChunk(0, static_cast<uint32_t>(cells), 1, gpu, receiverChunk);
        status = dispatchValid ? ValidateGIStorageBuffer(
            2 * ((cells + receiverChunk.itemCount - 1) / receiverChunk.itemCount), 16, gpu, argumentBytes) :
            GIResourceStatus::eInvalidInput;
    }
    const uint64_t frameBytes = cells * GI_FRAME_CELL_BYTES + GI_FRAME_FIXED_BYTES + argumentBytes;
    if (raysPerFace != 32 && raysPerFace != 64 && raysPerFace != 128)
    {
        status = GIResourceStatus::eInvalidInput;
    }
    if (status == GIResourceStatus::eSuccess)
    {
        uint64_t metadataBytes = 0;
        status                 = ValidateGIStorageBuffer(cells, 16, gpu, metadataBytes);
    }
    if (status == GIResourceStatus::eSuccess &&
        (frameBytes > UINT64_MAX - peakBytes ||
         receiverSurfaceBytes > UINT64_MAX - peakBytes - frameBytes))
    {
        status = GIResourceStatus::eOverflow;
    }
    if (status == GIResourceStatus::eSuccess)
    {
        const uint64_t base       = peakBytes + frameBytes + receiverSurfaceBytes;
        const uint32_t hitStride  = compactCache ? GI_COMPACT_HIT_BYTES : sizeof(GIHit);
        const uint64_t faceStride = raysPerFace * hitStride;
        const uint64_t range      = std::min(uint64_t(gpu.maxStorageBufferRange),
                                             uint64_t(std::numeric_limits<uint32_t>::max()));
        const uint64_t available  = budgetBytes > base ? budgetBytes - base : 0;
        capacity                  = static_cast<uint32_t>(
            std::min({cells, range / faceStride, available / (GI_FACE_COUNT * faceStride)}));
        peakBytes      = base + uint64_t(capacity) * GI_FACE_COUNT * faceStride;
        uint64_t bytes = 0;
        status         = capacity == 0 ?
            GIResourceStatus::eBudget :
            ValidateGIStorageBuffer(uint64_t(capacity) * raysPerFace, hitStride, gpu, bytes);
    }
    return status;
}

DynamicVoxelGIResourceEstimate EstimateDynamicVoxelGIResources(
    const DynamicVoxelGIResourceRequest& request,
    const RHIGPUInfo& gpu)
{
    DynamicVoxelGIResourceEstimate estimate;
    const DynamicVoxelGISettings& settings = request.settings;
    uint64_t cells                         = 0;
    if ((settings.resolution != 64 && settings.resolution != 128 && settings.resolution != 256) ||
        (settings.raysPerFace != 32 && settings.raysPerFace != 64 && settings.raysPerFace != 128) ||
        settings.memoryBudgetBytes == 0 ||
        (settings.neighborRadius != 1 && settings.neighborRadius != 2))
    {
        estimate.status = GIResourceStatus::eInvalidInput;
    }
    else
    {
        cells = uint64_t(settings.resolution) * settings.resolution * settings.resolution;
        if (request.staticCapacity > cells || request.dynamicCapacity > cells)
        {
            estimate.status = GIResourceStatus::eInvalidInput;
        }
        else if (settings.backend == VoxelGIQueryBackend::eHardwareRT)
        {
            // H0 must supply queried AS sizes and the validated triangle-hit layout.
            estimate.status = GIResourceStatus::eUnavailableBackend;
        }
    }
    if (estimate.status == GIResourceStatus::eSuccess)
    {
        const uint64_t staticRays  = request.staticCapacity * 6 * settings.raysPerFace;
        const uint64_t dynamicRays = request.dynamicCapacity * 6 * settings.raysPerFace;
        estimate.resources         = {
            {"owner", false, cells, 4, 2},
            {"albedo", false, cells, 4, 2},
            {"normal_metallic", false, cells, 4, 2},
            {"emission", false, cells, 8, 2},
            {"grid_to_list", true, cells, 4, 2},
            {"light_masks", true, cells, 4, 2},
            {"static_list", true, cells, 4, 1},
            {"dynamic_occupied_list", true, cells, 4, 1},
            {"occupied_counts", true, 2, 4, 1},
            {"dynamic_receiver_list", true, request.dynamicCapacity, 4, 1},
            {"active_static_list", true, request.staticCapacity, 4, 1},
            {"receiver_validity", true, request.staticCapacity + request.dynamicCapacity, 4, 1},
            {"static_visibility", true, staticRays, 8, 1},
            {"static_dynamic_visibility", true, staticRays, 8, 1},
            {"dynamic_visibility", true, dynamicRays, 8, 1},
            {"irradiance_history_scratch", false, cells, 8, 36},
            {"sender_environment", false, cells, 8, 2},
            {"dispatch_arguments", true, 3, 4, 2}};
        if (request.averagedReflectance)
        {
            estimate.resources.push_back({"reflectance_accumulation", true, cells, 16, 2});
            estimate.resources.push_back({"resolved_reflectance", false, cells, 4, 2});
        }
        if (request.coneTransition)
        {
            uint64_t mipCells = 0;
            for (uint32_t side = settings.resolution; side != 0; side /= 2)
            {
                mipCells += uint64_t(side) * side * side;
            }
            estimate.resources.push_back({"cone_owner_normal_emission_sky", false, cells, 24, 1});
            estimate.resources.push_back({"cone_albedo_radiance_mips", false, mipCells, 12, 1});
            if (request.averagedReflectance)
            {
                estimate.resources.push_back({"cone_reflectance_accumulation", true, cells, 16, 1});
                estimate.resources.push_back({"cone_reflectance", false, cells, 4, 1});
            }
        }
        estimate.peakBytes = request.retiringBytes;
        for (GIResourceEstimate& resource : estimate.resources)
        {
            if (resource.storageBuffer)
            {
                estimate.status = ValidateGIStorageBuffer(resource.elements, resource.stride, gpu,
                                                          resource.bytesPerAllocation);
            }
            else
            {
                resource.bytesPerAllocation = resource.elements * resource.stride;
            }
            const uint64_t bytes = resource.bytesPerAllocation * resource.copies;
            if (estimate.status == GIResourceStatus::eSuccess &&
                bytes > std::numeric_limits<uint64_t>::max() - estimate.peakBytes)
            {
                estimate.status = GIResourceStatus::eOverflow;
            }
            if (estimate.status != GIResourceStatus::eSuccess)
            {
                estimate.rejectedResource = resource.name;
                break;
            }
            estimate.peakBytes += bytes;
        }
        if (estimate.status == GIResourceStatus::eSuccess &&
            estimate.peakBytes > settings.memoryBudgetBytes)
        {
            estimate.status = GIResourceStatus::eBudget;
        }
    }
    return estimate;
}
} // namespace zen::rc
