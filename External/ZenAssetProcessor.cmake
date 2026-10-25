# Use the live sibling checkout during development, without pinning a revision.
# An explicit empty source path selects an installed package instead.
get_filename_component(ZEN_ASSET_PROCESSOR_DEFAULT_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../ZenAssetProcessor" ABSOLUTE)
if(NOT EXISTS "${ZEN_ASSET_PROCESSOR_DEFAULT_SOURCE_DIR}/CMakeLists.txt")
    set(ZEN_ASSET_PROCESSOR_DEFAULT_SOURCE_DIR "")
endif()
set(ZEN_ASSET_PROCESSOR_SOURCE_DIR "${ZEN_ASSET_PROCESSOR_DEFAULT_SOURCE_DIR}"
    CACHE PATH "Standalone ZenAssetProcessor checkout; empty selects an installed package")
unset(ZEN_ASSET_PROCESSOR_DEFAULT_SOURCE_DIR)

if(ZEN_ASSET_PROCESSOR_SOURCE_DIR)
    if(NOT EXISTS "${ZEN_ASSET_PROCESSOR_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "ZEN_ASSET_PROCESSOR_SOURCE_DIR must name a ZenAssetProcessor checkout")
    endif()
    set(ZENASSET_BUILD_PROCESSOR ${ZEN_BUILD_ASSET_TOOLS})
    set(ZENASSET_BUILD_TESTING OFF)
    set(ZENASSET_BUILD_CLI OFF)
    add_subdirectory("${ZEN_ASSET_PROCESSOR_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/External/ZenAssetProcessor")
else()
    set(ZEN_ASSET_COMPONENTS Format Reader)
    if(ZEN_BUILD_ASSET_TOOLS)
        list(APPEND ZEN_ASSET_COMPONENTS Processor)
    endif()
    find_package(ZenAsset 0.1 CONFIG REQUIRED COMPONENTS ${ZEN_ASSET_COMPONENTS})
endif()

if(NOT TARGET ZenAsset::Reader OR (ZEN_BUILD_ASSET_TOOLS AND NOT TARGET ZenAsset::Processor))
    message(FATAL_ERROR "The selected ZenAsset package does not contain the required reader/processor targets")
endif()
