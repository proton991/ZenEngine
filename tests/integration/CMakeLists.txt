add_executable(ResourceOwnershipTest ResourceOwnershipTests.cpp)
target_link_libraries(ResourceOwnershipTest PRIVATE ZenCore GTest::gtest_main)
add_test(NAME ResourceOwnershipTest COMMAND ResourceOwnershipTest)
set_tests_properties(ResourceOwnershipTest PROPERTIES LABELS "unit;integration")

include_directories("${ZEN_ENGINE_SOURCE_DIR}")
add_library(ZenVulkanTestMain STATIC CommonTest/VulkanTestMain.cpp)
add_library(ZenVulkanTestSupport INTERFACE)
target_link_libraries(ZenVulkanTestSupport INTERFACE volk vma spirv-reflect)
target_link_libraries(ZenVulkanTestMain PUBLIC ZenCore gtest ZenVulkanTestSupport)
add_executable(RuntimeLaunchTest CommonTest/RuntimeLaunchTests.cpp CommonTest/CameraPathTests.cpp
    ${ZEN_ENGINE_SOURCE_DIR}/ZenApp/CameraPath.cpp ${ZEN_ENGINE_SOURCE_DIR}/ZenApp/LightPath.cpp ${ZEN_ENGINE_SOURCE_DIR}/ZenApp/RuntimeJson.cpp)
target_link_libraries(RuntimeLaunchTest PRIVATE ZenCore gtest_main simdjson)
add_test(NAME RuntimeLaunchTest COMMAND RuntimeLaunchTest)
set_tests_properties(RuntimeLaunchTest PROPERTIES LABELS "unit" TIMEOUT 30)
if(ZEN_BUILD_ASSET_TOOLS)
    add_executable(SceneAssemblyTest CommonTest/SceneAssemblyTests.cpp)
    target_link_libraries(SceneAssemblyTest PRIVATE ZenCore ZenAsset::Processor gtest_main)
    add_test(NAME SceneAssemblyTest COMMAND SceneAssemblyTest)
    set_tests_properties(SceneAssemblyTest PROPERTIES LABELS "asset;integration;cpu" TIMEOUT 120)
endif()
# Explicit integration target: requires a Vulkan device and validation layer.
add_executable(RayQueryIntegrationTest CommonTest/RayQueryIntegrationTests.cpp)
target_link_libraries(RayQueryIntegrationTest PRIVATE ZenCore ZenVulkanTestMain)

if(NOT ZEN_RHI_USE_INSTALLED)
    add_executable(VulkanRHITest
        CommonTest/ShaderReflectionTests.cpp
    )
    target_include_directories(VulkanRHITest PRIVATE "${ZEN_RHI_SOURCE_DIR}/src")
    target_link_libraries(VulkanRHITest PRIVATE ZenCore gtest_main ZenVulkanTestSupport)
    add_executable(VulkanRHIIntegrationTest
        "${ZEN_RHI_SOURCE_DIR}/tests/VulkanValidationProbe.cpp"
        CommonTest/VulkanPresentationReviewTests.cpp
        CommonTest/RHIWindowThreadingTests.cpp
        CommonTest/VulkanPipelineIntegrationTests.cpp
        CommonTest/BindlessSceneCapacityIntegrationTests.cpp
    )
    target_include_directories(VulkanRHIIntegrationTest PRIVATE "${ZEN_RHI_SOURCE_DIR}/src" "${ZEN_RHI_SOURCE_DIR}/tests")
    target_link_libraries(VulkanRHIIntegrationTest PRIVATE ZenCore ZenVulkanTestMain ZenRHI::TestSupport)
    # The engine dispatch case uses the same shader output as ZenApp.
    target_compile_definitions(VulkanRHIIntegrationTest PRIVATE ZENRHI_TEST_SHADER_DIR="${ZEN_ENGINE_SOURCE_DIR}/Data/SpvShaders/")
    add_dependencies(VulkanRHIIntegrationTest SpvShaders)
    add_test(NAME VulkanRHITest COMMAND VulkanRHITest)
    set_tests_properties(VulkanRHITest PROPERTIES LABELS "unit" TIMEOUT 120)
    add_test(NAME VulkanRHIIntegrationTest COMMAND VulkanRHIIntegrationTest)
    set_tests_properties(VulkanRHIIntegrationTest PROPERTIES LABELS "vulkan;integration" TIMEOUT 300)

    # Native acceptance tests deliberately inspect the private window adapter.
    target_include_directories(VulkanRHIIntegrationTest PRIVATE ${ZEN_ENGINE_SOURCE_DIR}/ZenCore/Source)
    target_link_libraries(VulkanRHIIntegrationTest PRIVATE ZenWindowBackendInternal)
endif()

add_executable(RenderCoreTest
    RenderCoreTest/RenderCoreTests.cpp
    RenderCoreTest/RDGMetricsTests.cpp
    RenderCoreTest/RDGProfilingTests.cpp
    RenderCoreTest/VoxelResourcePlanningTests.cpp
    RenderCoreTest/VoxelGISettingsTests.cpp
)
target_include_directories(RenderCoreTest PRIVATE
    ${ZEN_ENGINE_SOURCE_DIR}/ZenCore/Include
)
target_compile_definitions(RenderCoreTest PRIVATE
    $<TARGET_PROPERTY:ZenCore,INTERFACE_COMPILE_DEFINITIONS>
)
target_link_libraries(RenderCoreTest PRIVATE ZenRenderCoreTestSources ZenRHI::TestSupport spdlog::spdlog gli glm spirv-reflect)
# The shared fake-backend test translation unit exceeds the standard COFF section limit.
set_source_files_properties(RenderCoreTest/RenderCoreTests.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")
add_dependencies(RenderCoreTest SpvShaders)
add_executable(EncoderAllocationBenchmark RenderCoreTest/EncoderAllocationBenchmark.cpp)
target_link_libraries(EncoderAllocationBenchmark PRIVATE ZenCore)
add_executable(CommonTest
    CommonTest/CameraInputTests.cpp
    CommonTest/CleanupRegressionTests.cpp
)

# Native Cone visibility regression, isolated from suite-global Vulkan fixtures.
add_executable(ConeVoxelGIIntegrationTest CommonTest/ConeVoxelGIIntegrationTests.cpp
    CommonTest/VoxelReflectanceIntegrationTests.cpp)
target_link_libraries(ConeVoxelGIIntegrationTest PRIVATE ZenCore ZenVulkanTestMain)
add_dependencies(ConeVoxelGIIntegrationTest SpvShaders)
set(SCENE_CULLING_READBACK_SHADERS)
foreach(SHADER scene_gbuffer_readback scene_indirect_readback)
  set(OUTPUT "${ZEN_ENGINE_SOURCE_DIR}/Data/SpvShaders/Tests/${SHADER}.comp.spv")
  add_custom_command(OUTPUT "${OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ZEN_ENGINE_SOURCE_DIR}/Data/SpvShaders/Tests"
    COMMAND ${GLSL_VALIDATOR} --target-env spirv1.3 -V
            "${CMAKE_CURRENT_SOURCE_DIR}/CommonTest/Shaders/${SHADER}.comp"
            -o "${OUTPUT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/CommonTest/Shaders/${SHADER}.comp" VERBATIM)
  list(APPEND SCENE_CULLING_READBACK_SHADERS "${OUTPUT}")
endforeach()
add_custom_target(SceneCullingTestShaders DEPENDS ${SCENE_CULLING_READBACK_SHADERS})
add_dependencies(ConeVoxelGIIntegrationTest SceneCullingTestShaders)

target_link_libraries(CommonTest PRIVATE ZenCore gtest_main)
target_include_directories(CommonTest PRIVATE ${ZEN_ENGINE_SOURCE_DIR}/ZenCore/Source)

foreach(TEST_TARGET CommonTest RenderCoreTest)
    add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})
    set_tests_properties(${TEST_TARGET} PROPERTIES LABELS "unit" TIMEOUT 120)
endforeach()
add_executable(WindowPlatformTest CommonTest/WindowPlatformTests.cpp)
target_link_libraries(WindowPlatformTest PRIVATE ZenCore ZenWindowBackendInternal ZenVulkanTestMain)
add_test(NAME WindowPlatformTest COMMAND WindowPlatformTest)
set_tests_properties(WindowPlatformTest PROPERTIES LABELS "native" TIMEOUT 60)

if(TARGET ZenEditorModel)
    add_subdirectory(ZenEditor)
endif()
if(TARGET ZenUI)
    add_subdirectory(ZenUI)
endif()
option(ZEN_BUILD_ASSET_INTEGRATION_TESTS "Test cooked assets through the engine" OFF)
if(ZEN_BUILD_ASSET_INTEGRATION_TESTS)
    add_subdirectory(Assets)
endif()

if(TARGET ZenApp AND ZEN_BUILD_ASSET_TOOLS)
  add_executable(MakeRuntimeFixture ZenApp/MakeRuntimeFixture.cpp)
  target_link_libraries(MakeRuntimeFixture PRIVATE ZenEngineAssetTools)
  target_compile_definitions(MakeRuntimeFixture PRIVATE
    ZEN_RUNTIME_FIXTURE="${ZEN_ENGINE_SOURCE_DIR}/tests/integration/ZenEditor/Fixtures/inspection.gltf")
  add_test(NAME ZenAppCacheInputAliases
    COMMAND ${CMAKE_COMMAND} -DZEN_APP=$<TARGET_FILE:ZenApp> -DFIXTURE=$<TARGET_FILE:MakeRuntimeFixture>
      -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cache-input-aliases
      -P ${CMAKE_CURRENT_SOURCE_DIR}/ZenApp/CacheInputAliases.cmake)
  set_tests_properties(ZenAppCacheInputAliases PROPERTIES LABELS "integration" TIMEOUT 45)
endif()
