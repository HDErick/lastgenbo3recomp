# bo3_zm - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bo3_zm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BO3ZM_SOURCES
    src/main.cpp
    src/crash_reporter.cpp
)

if(WIN32)
    add_executable(bo3_zm WIN32 ${BO3ZM_SOURCES})
else()
    add_executable(bo3_zm ${BO3ZM_SOURCES})
endif()

rexglue_setup_target(bo3_zm GPU_PLUGINS xenos)

# rex_app.cpp includes the console overlay headers; the SDK keeps ImGui private.
target_include_directories(bo3_zm PRIVATE
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(bo3_zm PRIVATE dbghelp)
target_link_options(bo3_zm PRIVATE "-Xlinker" "/DEBUG")

# Separate guest executable, sharing the SDK and installed content.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/multiplayer/generated/default/sources.cmake")
    add_subdirectory(multiplayer)
endif()
