cmake_minimum_required(VERSION 3.28)

project(ProjectPredation
    VERSION 0.1.0
    DESCRIPTION "Project Predation - first-person multiplayer horror extraction game"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global options
# ---------------------------------------------------------------------------
option(PRED_BUILD_TESTS "Build unit tests" ON)
option(PRED_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PredWarnings)
include(PredShaders)

if(MSVC)
    # Parallel compilation, UTF-8 sources, strict conformance.
    add_compile_options(/MP /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# All targets put their runtime output in one place so the game can find
# generated assets and so debugging from VS Code is predictable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Where build-time generated assets (compiled shaders) go. The runtime adds
# this directory as an asset search root in development builds.
set(PRED_GENERATED_ASSETS_DIR "${CMAKE_BINARY_DIR}/GeneratedAssets")

add_subdirectory(Engine)
add_subdirectory(Game)

if(PRED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()
