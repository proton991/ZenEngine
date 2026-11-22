# Opt-in tests of packages through the engine reader, renderer and editor.
set(ZEN_ASSET_TEST_CORPUS "${ZEN_ASSET_PROCESSOR_SOURCE}/tests/fixtures" CACHE PATH "Processor semantic fixture corpus")
if(NOT EXISTS "${ZEN_ASSET_TEST_CORPUS}/complete_scene.glb")
    message(FATAL_ERROR "Set ZEN_ASSET_TEST_CORPUS to the processor test corpus")
endif()
add_compile_definitions(ZEN_ASSET_TEST_CORPUS="${ZEN_ASSET_TEST_CORPUS}")
if(NOT TARGET ZenEngineAssetTools OR NOT TARGET gtest_main)
    message(FATAL_ERROR "Configure ZenEngine with BUILD_TESTING=ON, ZEN_BUILD_ASSET_TOOLS=ON and ZEN_BUILD_ASSET_INTEGRATION_TESTS=ON")
endif()

add_executable(ZenAssetEngineTests
    CommonTest/SceneModelCatalogTests.cpp
    CommonTest/SceneImportTests.cpp
    CommonTest/SceneAssetIOTests.cpp
    CommonTest/SceneLoaderTests.cpp
    CommonTest/FbxSceneImportTests.cpp
    CommonTest/SceneAnimationTests.cpp
    CommonTest/SceneImportCompatibilityTests.cpp
    CommonTest/SceneImportStressTests.cpp
    CommonTest/SceneImportMorphAttributeTests.cpp
    CommonTest/ScenePhysicalUnitsTests.cpp
    CommonTest/SceneImportPaddingTests.cpp
    CommonTest/SceneAnimationPointerTestSupport.cpp
    CommonTest/SceneAnimationPointerTests.cpp
    CommonTest/ProcessedSceneAdapterTests.cpp
    CommonTest/AssetImportRegressionTests.cpp)
target_link_libraries(ZenAssetEngineTests PRIVATE ZenEngineAssetTools ZenAsset::Processor gtest_main draco::draco)
target_include_directories(ZenAssetEngineTests PRIVATE "${PROJECT_SOURCE_DIR}/ZenCore/Source")
if(TARGET ZenAssetUfbx)
    target_link_libraries(ZenAssetEngineTests PRIVATE ZenAssetUfbx meshoptimizer)
    FetchContent_GetProperties(draco SOURCE_DIR ZEN_TEST_DRACO_SOURCE_DIR BINARY_DIR ZEN_TEST_DRACO_BINARY_DIR)
    target_include_directories(ZenAssetEngineTests PRIVATE "${ZEN_TEST_DRACO_SOURCE_DIR}/src"
        "${ZEN_TEST_DRACO_BINARY_DIR}" "${PROJECT_BINARY_DIR}")
else()
    target_link_libraries(ZenAssetEngineTests PRIVATE ZenAsset::PrivateUfbx meshoptimizer::meshoptimizer)
endif()
add_test(NAME ZenAssetEngineTests COMMAND ZenAssetEngineTests)
set_tests_properties(ZenAssetEngineTests PROPERTIES LABELS "asset;integration;cpu" TIMEOUT 600)

foreach(NAME GLTFCorpusImport SceneLoadBenchmark ScenePackageSmoke ScenePackageBenchmark)
    add_executable(${NAME} "CommonTest/${NAME}.cpp")
    target_link_libraries(${NAME} PRIVATE ZenCore ZenAsset::Reader)
endforeach()
target_link_libraries(GLTFCorpusImport PRIVATE ZenEngineAssetTools)
target_link_libraries(SceneLoadBenchmark PRIVATE ZenEngineAssetTools)
if(NOT ZEN_RHI_USE_INSTALLED)
    add_executable(SceneUploadBenchmark CommonTest/SceneUploadBenchmark.cpp)
    target_link_libraries(SceneUploadBenchmark PRIVATE ZenCore ZenAsset::Reader ZenVulkanTestSupport)
    target_include_directories(SceneUploadBenchmark PRIVATE "${ZEN_RHI_SOURCE_DIR}/src")
endif()

add_executable(ZenAssetTextureIntegrationTest CommonTest/ArraySceneTextureIntegrationTests.cpp)
target_link_libraries(ZenAssetTextureIntegrationTest PRIVATE ZenCore ZenVulkanTestMain ZenRHI::TestSupport)
target_include_directories(ZenAssetTextureIntegrationTest PRIVATE "${PROJECT_SOURCE_DIR}/tests/integration/CommonTest")
set(ASSET_SHADER_PATH "${CMAKE_CURRENT_BINARY_DIR}/shaders/")
set(ASSET_SHADER_BINARIES)
foreach(SHADER scene_array_views.comp scene_spatial_views.comp scene_normal_sampling.comp spatial_alpha.vert spatial_alpha_hardware.comp spatial_alpha_hardware.frag)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/CommonTest/Shaders/${SHADER}")
    if(SHADER MATCHES "^spatial_alpha")
        set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Editor/Shaders/${SHADER}")
    endif()
    set(OUTPUT "${ASSET_SHADER_PATH}${SHADER}.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSET_SHADER_PATH}"
        COMMAND ${GLSL_VALIDATOR} --target-env spirv1.3 -V "${SOURCE}"
            "-I${PROJECT_SOURCE_DIR}/ZenCore/Include" "-I${PROJECT_SOURCE_DIR}/Data/Shaders" "-I${ZEN_RHI_INCLUDE_DIR}" -o "${OUTPUT}" --depfile "${OUTPUT}.d"
        DEPENDS "${SOURCE}" "${PROJECT_SOURCE_DIR}/Data/Shaders/Common/material.glsl"
            "${PROJECT_SOURCE_DIR}/Data/Shaders/Common/bindless_heap.glsl"
            "${ZEN_RHI_INCLUDE_DIR}/ZenRHI/Shader/Bindless.h" DEPFILE "${OUTPUT}.d" VERBATIM)
    list(APPEND ASSET_SHADER_BINARIES "${OUTPUT}")
endforeach()
add_custom_target(ZenAssetIntegrationShaders DEPENDS ${ASSET_SHADER_BINARIES})
add_dependencies(ZenAssetTextureIntegrationTest ZenAssetIntegrationShaders)
target_compile_definitions(ZenAssetTextureIntegrationTest PRIVATE RDG_REFLECTION_TEST_PATH="${ASSET_SHADER_PATH}")
add_test(NAME ZenAssetTextureIntegrationTest COMMAND ZenAssetTextureIntegrationTest)
set_tests_properties(ZenAssetTextureIntegrationTest PROPERTIES LABELS "asset;integration;vulkan" TIMEOUT 300)

if(TARGET ZenEditorServices)
    add_executable(EditorAssetRenderingTest Editor/EditorAssetRenderingTests.cpp)
    target_link_libraries(EditorAssetRenderingTest PRIVATE ZenVulkanTestSupport ZenRHI::TestSupport ZenEditorServices gtest_main)
    target_include_directories(EditorAssetRenderingTest PRIVATE "${PROJECT_SOURCE_DIR}/tests/integration/ZenEditor"
        "${PROJECT_SOURCE_DIR}/tests/integration/CommonTest" CommonTest)
    target_compile_definitions(EditorAssetRenderingTest PRIVATE
        ZEN_EDITOR_FIXTURES="${PROJECT_SOURCE_DIR}/tests/integration/ZenEditor/Fixtures/"
        ZEN_EDITOR_SHARED_FIXTURES="${ZEN_ASSET_TEST_CORPUS}/"
        ZEN_ASSET_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Editor/Fixtures/")
    add_dependencies(EditorAssetRenderingTest ZenAssetIntegrationShaders)
    target_compile_definitions(EditorAssetRenderingTest PRIVATE ZEN_ASSET_SHADER_PATH="${ASSET_SHADER_PATH}")
    add_test(NAME EditorAssetRenderingTest COMMAND EditorAssetRenderingTest)
    set_tests_properties(EditorAssetRenderingTest PROPERTIES LABELS "asset;integration;vulkan" TIMEOUT 600)
endif()
