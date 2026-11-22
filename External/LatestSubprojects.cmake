include_guard(GLOBAL)
# Root subprojects follow their .gitmodules branches unless updates are disabled.
option(ZEN_UPDATE_SUBPROJECTS "Fetch the latest root subprojects during configuration" ON)

function(zen_update_subproject NAME OPTION)
    set(CHECKOUT "${CMAKE_CURRENT_SOURCE_DIR}/${NAME}")
    if(NOT EXISTS "${CHECKOUT}/CMakeLists.txt")
        message(FATAL_ERROR "Initialize ${NAME} with git submodule update --init --recursive, or disable ${OPTION}")
    endif()
    if(ZEN_UPDATE_SUBPROJECTS)
        find_package(Git REQUIRED)
        # Build trees share these checkouts, so one git update runs at a time.
        set(LOCK "${CMAKE_CURRENT_SOURCE_DIR}/External/.dependencies")
        file(LOCK "${LOCK}" DIRECTORY GUARD FUNCTION TIMEOUT 1800)
        execute_process(COMMAND "${GIT_EXECUTABLE}" config --file "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules"
            --get "submodule.${NAME}.branch" OUTPUT_VARIABLE BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" fetch --quiet origin
            "+refs/heads/${BRANCH}:refs/remotes/origin/${BRANCH}" COMMAND_ERROR_IS_FATAL ANY)
        # Developers work in these checkouts. A fast-forward keeps uncommitted edits and refuses
        # anything that would replace local work.
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" merge --quiet --ff-only "origin/${BRANCH}"
            RESULT_VARIABLE MERGE_RESULT ERROR_VARIABLE MERGE_ERROR)
        if(MERGE_RESULT)
            message(WARNING "${NAME} could not fast-forward to origin/${BRANCH} to keep local work: ${MERGE_ERROR}")
        endif()
        file(LOCK "${LOCK}" DIRECTORY RELEASE)
    endif()
endfunction()

# A source directory that is the root submodule follows its configured branch.
# Any other checkout is an explicit override and stays as it is.
function(zen_update_subproject_checkout NAME SOURCE_DIR)
    get_filename_component(CHECKOUT "${SOURCE_DIR}" REALPATH)
    if(CHECKOUT STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/${NAME}")
        zen_update_subproject(${NAME} ZEN_UPDATE_SUBPROJECTS)
    endif()
endfunction()

# Zen components are released together under one version. A checkout from another release fails
# here, before it can fail at compile or link time.
function(zen_require_release_version NAME VERSION)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" RELEASE "${VERSION}")
    if(NOT RELEASE STREQUAL "${ZenEngine_VERSION_MAJOR}.${ZenEngine_VERSION_MINOR}")
        message(FATAL_ERROR "${NAME} ${VERSION} does not match ZenEngine ${ZenEngine_VERSION}. "
                            "Check out the ${NAME} revision of this release.")
    endif()
endfunction()

# Source checkouts report the version their own project() declares.
function(zen_source_version OUTPUT SOURCE_DIR)
    get_directory_property(VERSION DIRECTORY "${SOURCE_DIR}" DEFINITION PROJECT_VERSION)
    set(${OUTPUT} "${VERSION}" PARENT_SCOPE)
endfunction()

function(zen_add_latest_subproject NAME OPTION)
    zen_update_subproject(${NAME} ${OPTION})
    add_subdirectory(${NAME})
endfunction()
