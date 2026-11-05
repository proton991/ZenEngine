# Follow the development branch over SSH; configure again to fetch new commits.
# Local source and installed-package modes remain explicit alternatives.
set(ZEN_ASSET_PROCESSOR_SOURCE_DIR "" CACHE PATH
    "Optional ZenAssetProcessor source override; empty fetches GitHub main")
option(ZEN_ASSET_PROCESSOR_USE_INSTALLED "Use an installed ZenAsset package instead of source" OFF)

# Build trees share one checkout, so it cannot use FetchContent: its per-tree clone state makes a
# new tree delete the checkout. Git runs under a lock, and a failed clone or fetch stops
# configuration with any existing checkout intact.
function(zen_update_asset_processor CHECKOUT)
    find_package(Git REQUIRED)
    set(FETCH_DIR "${CHECKOUT}.fetch")
    file(MAKE_DIRECTORY "${FETCH_DIR}")
    file(LOCK "${FETCH_DIR}" DIRECTORY GUARD FUNCTION TIMEOUT 600)
    if(EXISTS "${CHECKOUT}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" status --porcelain
            OUTPUT_VARIABLE LOCAL_CHANGES COMMAND_ERROR_IS_FATAL ANY)
        if(LOCAL_CHANGES)
            message(FATAL_ERROR "${CHECKOUT} has local changes and cannot follow GitHub main. Move them "
                "to a separate checkout and set ZEN_ASSET_PROCESSOR_SOURCE_DIR to develop against it.")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" fetch --quiet --depth 1 origin
            "+refs/heads/main:refs/remotes/origin/main" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" checkout --quiet --detach origin/main
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        set(CLONE "${FETCH_DIR}/clone")
        file(REMOVE_RECURSE "${CLONE}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" clone --quiet --depth 1 --branch main
            "git@github.com:proton991/ZenAssetProcessor.git" "${CLONE}" COMMAND_ERROR_IS_FATAL ANY)
        file(RENAME "${CLONE}" "${CHECKOUT}")
    endif()
endfunction()

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
        set(ZEN_ASSET_PROCESSOR_SOURCE "${ZEN_ASSET_PROCESSOR_SOURCE_DIR}")
    else()
        set(ZEN_ASSET_PROCESSOR_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ZenAssetProcessor")
        zen_update_asset_processor("${ZEN_ASSET_PROCESSOR_SOURCE}")
    endif()
    add_subdirectory("${ZEN_ASSET_PROCESSOR_SOURCE}" "${PROJECT_BINARY_DIR}/External/ZenAssetProcessor")
endif()

if(NOT TARGET ZenAsset::Reader OR (ZEN_BUILD_ASSET_TOOLS AND NOT TARGET ZenAsset::Processor))
    message(FATAL_ERROR "The selected ZenAsset package does not contain the required reader/processor targets")
endif()
