#include "Editor/Services/EditorController.h"
#include "Editor/Model/EditorText.h"
#include "SyntheticEnvironment.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "ZenRHI/RHIShaderUtil.h"
#include <volk.h>
#include "ZenRHI/Vulkan/VulkanInterop.h"
#include "SpatialQualificationFailure.h"
#include "Platform/FileSystem.h"
#include "SceneGraph/Texture.h"
#include "AssetLib/SceneAssetIO.h"
#include "AssetTools/SceneTools.h"
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <gli/gli.hpp>

namespace zen::editor
{
namespace
{
#include "EditorRenderingTestSupport.inl"
#include "ProcessedSceneRenderingTestSupport.inl"
#include "ArraySceneRenderingTests.inl"
#include "SpatialRayRenderingTests.inl"
#include "SpatialQualificationRenderingTests.inl"
#include "SourceProcessingRenderingTests.inl"
#include "SpatialGIRenderingTests.inl"
#include "ProcessedGeometryRenderingTests.inl"

INSTANTIATE_TEST_SUITE_P(Execution, EditorRendering, testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
} // namespace zen::editor
