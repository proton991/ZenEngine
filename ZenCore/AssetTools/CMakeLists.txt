add_library(ZenEngineAssetTools STATIC
    Include/AssetTools/SceneTools.h
    Source/SceneSourceImporter.cpp
    Source/ProcessedSourceImport.h
    Source/SceneTools.cpp
    Source/SceneAssetCache.cpp
    Source/SceneAssetCache.h
)
target_compile_features(ZenEngineAssetTools PUBLIC cxx_std_20)
target_include_directories(ZenEngineAssetTools
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Include"
    PRIVATE "${PROJECT_SOURCE_DIR}/ZenCore/Source"
            "${CMAKE_CURRENT_BINARY_DIR}/Generated"
)
target_compile_definitions(ZenEngineAssetTools PRIVATE ZEN_SCENE_CACHE_PATH="${PROJECT_SOURCE_DIR}/Cache/SceneAssets")
target_link_libraries(ZenEngineAssetTools PUBLIC ZenCore PRIVATE ZenAsset::Processor)
file(GLOB_RECURSE ZEN_ASSET_TOOL_FINGERPRINT_INPUTS CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/ZenCore/Include/AssetLib/*.h"
    "${PROJECT_SOURCE_DIR}/ZenCore/Source/AssetLib/*.h"
    "${PROJECT_SOURCE_DIR}/ZenCore/Source/AssetLib/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
)
list(SORT ZEN_ASSET_TOOL_FINGERPRINT_INPUTS)
string(REPLACE ";" "\n" ZEN_ASSET_TOOL_FINGERPRINT_INPUT_TEXT "${ZEN_ASSET_TOOL_FINGERPRINT_INPUTS}")
set(ZEN_ASSET_TOOL_FINGERPRINT_LIST "${CMAKE_CURRENT_BINARY_DIR}/SceneImportFingerprintInputs.txt")
file(CONFIGURE OUTPUT "${ZEN_ASSET_TOOL_FINGERPRINT_LIST}" CONTENT "@ZEN_ASSET_TOOL_FINGERPRINT_INPUT_TEXT@" @ONLY)
set(ZEN_ASSET_TOOL_FINGERPRINT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/Generated/AssetLib/SceneImportFingerprint.h")
add_custom_command(OUTPUT "${ZEN_ASSET_TOOL_FINGERPRINT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        "-DOUTPUT=${ZEN_ASSET_TOOL_FINGERPRINT_HEADER}"
        "-DINPUT_LIST=${ZEN_ASSET_TOOL_FINGERPRINT_LIST}"
        -P "${PROJECT_SOURCE_DIR}/ZenCore/CMake/SceneImportFingerprint.cmake"
    DEPENDS ${ZEN_ASSET_TOOL_FINGERPRINT_INPUTS} "${ZEN_ASSET_TOOL_FINGERPRINT_LIST}"
            "${PROJECT_SOURCE_DIR}/ZenCore/CMake/SceneImportFingerprint.cmake"
    VERBATIM
)
add_custom_target(ZenAssetToolFingerprint DEPENDS "${ZEN_ASSET_TOOL_FINGERPRINT_HEADER}")
add_dependencies(ZenEngineAssetTools ZenAssetToolFingerprint)
