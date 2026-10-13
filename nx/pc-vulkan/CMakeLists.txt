cmake_minimum_required(VERSION 3.25)
project(bo3_zm_vulkan LANGUAGES C CXX)
get_filename_component(BO3_ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REXGLUE_PLATFORM_SWITCH OFF CACHE BOOL "Desktop validation" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Match NX graphics backend" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "Vulkan-only baseline" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Initial baseline" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "Initial baseline" FORCE)
add_subdirectory("${BO3_ROOT}/tools/nx-sdk" rexglue-sdk)
add_subdirectory("${BO3_ROOT}/nx/native" bo3-native)
# The prepared header supplies explicit weak aliases required by SDK 0.10,
# which no longer defines the old REX_WEAK_FUNC macro. Clang accepts these too.
include("${BO3_ROOT}/build/nx-generated/zombies/sources.cmake")
add_executable(bo3_zm_vulkan WIN32 main.cpp draw_capture.cpp d3d_trace.cpp "${BO3_ROOT}/app/src/crash_reporter.cpp" ${GENERATED_SOURCES})
target_include_directories(bo3_zm_vulkan PRIVATE
    "${BO3_ROOT}/build/nx-generated/zombies" "${BO3_ROOT}/app/src"
    "${BO3_ROOT}/tools/nx-sdk/thirdparty/renderdoc"
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(bo3_zm_vulkan PRIVATE rex::runtime dbghelp)
target_link_options(bo3_zm_vulkan PRIVATE "-Xlinker" "/DEBUG")
target_compile_options(bo3_zm_vulkan PRIVATE -ffp-contract=off -fno-strict-aliasing)
rexglue_configure_target(bo3_zm_vulkan GPU_PLUGINS xenos)
add_dependencies(bo3_zm_vulkan rexgpu-bo3native)
set_target_properties(bo3_zm_vulkan PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
