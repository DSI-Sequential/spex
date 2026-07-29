# Generates <build>/generated/Version.h from version.toml and the current git
# state. Run both at configure time and as a build-time custom target so the
# git hash stays current on every build.
#
# Required cache/-D variables:
#   SPEX_SRC_DIR  - project source directory (contains version.toml, cmake/)
#   SPEX_BIN_DIR  - project binary/build directory

if (NOT DEFINED SPEX_SRC_DIR OR NOT DEFINED SPEX_BIN_DIR)
    message(FATAL_ERROR "GenerateVersion.cmake requires SPEX_SRC_DIR and SPEX_BIN_DIR")
endif ()

# --- Parse version.toml -----------------------------------------------------
file(READ "${SPEX_SRC_DIR}/version.toml" _version_toml)

set(SPEX_VERSION_MAJOR 0)
set(SPEX_VERSION_MINOR 0)
set(SPEX_VERSION_PATCH 0)

if (_version_toml MATCHES "major[ \t]*=[ \t]*([0-9]+)")
    set(SPEX_VERSION_MAJOR "${CMAKE_MATCH_1}")
endif ()
if (_version_toml MATCHES "minor[ \t]*=[ \t]*([0-9]+)")
    set(SPEX_VERSION_MINOR "${CMAKE_MATCH_1}")
endif ()
if (_version_toml MATCHES "patch[ \t]*=[ \t]*([0-9]+)")
    set(SPEX_VERSION_PATCH "${CMAKE_MATCH_1}")
endif ()

set(SPEX_VERSION_STRING "${SPEX_VERSION_MAJOR}.${SPEX_VERSION_MINOR}.${SPEX_VERSION_PATCH}")

# --- Query git --------------------------------------------------------------
find_package(Git QUIET)
set(SPEX_GIT_HASH "unknown")

if (GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SPEX_SRC_DIR}"
        OUTPUT_VARIABLE _git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_result)

    if (_git_result EQUAL 0 AND _git_hash)
        set(SPEX_GIT_HASH "${_git_hash}")

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" diff --quiet --ignore-submodules HEAD
            WORKING_DIRECTORY "${SPEX_SRC_DIR}"
            RESULT_VARIABLE _git_dirty
            ERROR_QUIET)

        if (NOT _git_dirty EQUAL 0)
            set(SPEX_GIT_HASH "${SPEX_GIT_HASH}-dirty")
        endif ()
    endif ()
endif ()

# --- Emit header (only rewrites when content changes) -----------------------
configure_file(
    "${SPEX_SRC_DIR}/cmake/Version.h.in"
    "${SPEX_BIN_DIR}/generated/Version.h"
    @ONLY)
