cmake_minimum_required(VERSION 3.22)

# Parse version.toml so the CMake project version matches the app version.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.toml" _spex_version_toml)
set(_spex_major 0)
set(_spex_minor 0)
set(_spex_patch 0)
if (_spex_version_toml MATCHES "major[ \t]*=[ \t]*([0-9]+)")
    set(_spex_major "${CMAKE_MATCH_1}")
endif ()
if (_spex_version_toml MATCHES "minor[ \t]*=[ \t]*([0-9]+)")
    set(_spex_minor "${CMAKE_MATCH_1}")
endif ()
if (_spex_version_toml MATCHES "patch[ \t]*=[ \t]*([0-9]+)")
    set(_spex_patch "${CMAKE_MATCH_1}")
endif ()

project(spex VERSION ${_spex_major}.${_spex_minor}.${_spex_patch} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.2
)
FetchContent_MakeAvailable(juce)

juce_add_gui_app(spex
    PRODUCT_NAME "spex"
)

# ---------------------------------------------------------------------------
# Version header (generated from version.toml + git hash).
# Generated once now so IDE indexing works, then regenerated on every build so
# the embedded git hash always reflects the current commit.
# ---------------------------------------------------------------------------
set(SPEX_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DSPEX_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DSPEX_BIN_DIR=${CMAKE_BINARY_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateVersion.cmake")

add_custom_target(spex_version
    BYPRODUCTS "${SPEX_GENERATED_DIR}/Version.h"
    COMMAND "${CMAKE_COMMAND}"
        -DSPEX_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DSPEX_BIN_DIR=${CMAKE_BINARY_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateVersion.cmake"
    COMMENT "Generating spex version header"
    VERBATIM)

add_dependencies(spex spex_version)

target_include_directories(spex PRIVATE "${SPEX_GENERATED_DIR}")

target_sources(spex PRIVATE
    main.cpp
    MainComponent.cpp
)

target_compile_definitions(spex PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_compile_options(spex PRIVATE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2>
    $<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O2>
)

target_link_libraries(spex PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
)
