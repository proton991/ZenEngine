include_guard(GLOBAL)
# Root subprojects follow GitHub main unless updates are disabled.
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
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" fetch --quiet origin
            "+refs/heads/main:refs/remotes/origin/main" COMMAND_ERROR_IS_FATAL ANY)
        # Developers work in these checkouts. A fast-forward keeps uncommitted edits and refuses
        # anything that would replace local work.
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" merge --quiet --ff-only origin/main
            RESULT_VARIABLE MERGE_RESULT ERROR_VARIABLE MERGE_ERROR)
        if(MERGE_RESULT)
            message(WARNING "${NAME} stays behind GitHub main to keep local work: ${MERGE_ERROR}")
        endif()
        file(LOCK "${LOCK}" DIRECTORY RELEASE)
    endif()
endfunction()

function(zen_add_latest_subproject NAME OPTION)
    zen_update_subproject(${NAME} ${OPTION})
    add_subdirectory(${NAME})
endfunction()
