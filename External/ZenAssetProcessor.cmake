# Follow the development branch over SSH; configure again to fetch new commits.
# Local source and installed-package modes remain explicit alternatives.
set(ZEN_ASSET_PROCESSOR_SOURCE_DIR "" CACHE PATH
    "Optional ZenAssetProcessor source override; empty fetches GitHub main")
option(ZEN_ASSET_PROCESSOR_USE_INSTALLED "Use an installed ZenAsset package instead of source" OFF)

if(ZEN_ASSET_PROCESSOR_USE_INSTALLED)
    set(ZEN_ASSET_COMPONENTS Format Reader)
    if(ZEN_BUILD_ASSET_TOOLS)
        list(APPEND ZEN_ASSET_COMPONENTS Processor)
    endif()
    find_package(ZenAsset CONFIG REQUIRED COMPONENTS ${ZEN_ASSET_COMPONENTS})
else()
    set(ZENASSET_BUILD_PROCESSOR ${ZEN_BUILD_ASSET_TOOLS})
    set(ZENASSET_BUILD_TESTING OFF)
    set(ZENASSET_BUILD_CLI OFF)
    if(ZEN_ASSET_PROCESSOR_SOURCE_DIR)
        if(NOT EXISTS "${ZEN_ASSET_PROCESSOR_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "ZEN_ASSET_PROCESSOR_SOURCE_DIR must name a ZenAssetProcessor checkout")
        endif()
        add_subdirectory("${ZEN_ASSET_PROCESSOR_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/External/ZenAssetProcessor")
    else()
        include(FetchContent)
        FetchContent_Declare(zen_asset_processor
            GIT_REPOSITORY "git@github.com:proton991/ZenAssetProcessor.git"
            GIT_TAG origin/main
            GIT_SHALLOW TRUE
            GIT_REMOTE_UPDATE_STRATEGY CHECKOUT
            BINARY_DIR "${PROJECT_BINARY_DIR}/External/ZenAssetProcessor")
        FetchContent_MakeAvailable(zen_asset_processor)
    endif()
endif()

if(NOT TARGET ZenAsset::Reader OR (ZEN_BUILD_ASSET_TOOLS AND NOT TARGET ZenAsset::Processor))
    message(FATAL_ERROR "The selected ZenAsset package does not contain the required reader/processor targets")
endif()
