# A separate GPU plugin: SDK guest command parsing, BO3 native Vulkan draws.
set(BO3_SDK "${BO3_ROOT}/tools/nx-sdk")
set(BO3_TRANSLATOR "${BO3_ROOT}/tools/bo3-mw2-shader-translator/runtime/gpu/vulkan")
if(NOT EXISTS "${BO3_TRANSLATOR}/shader_translator.cpp")
    message(FATAL_ERROR "Run prepare-mw2-shader-tools.py --adapt-bo3 first")
endif()
add_library(rexgpu-bo3native SHARED
    native_gpu.cpp
    rectangle_shader.cpp
    texture_upload.cpp
    color_resolve.cpp
    "${BO3_SDK}/thirdparty/crypto/sha256.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/present_spirv.h"
    "${BO3_TRANSLATOR}/shader_translator.cpp" "${BO3_TRANSLATOR}/spirv.cpp"
    "${BO3_SDK}/src/graphics/graphics_system.cpp"
    "${BO3_SDK}/src/graphics/command_processor.cpp"
    "${BO3_SDK}/src/graphics/register_file.cpp"
    "${BO3_SDK}/src/graphics/registers.cpp"
    "${BO3_SDK}/src/graphics/flags.cpp"
    "${BO3_SDK}/src/graphics/format/ucode.cpp"
    "${BO3_SDK}/src/graphics/sampler_info.cpp"
    "${BO3_SDK}/src/graphics/util/draw.cpp"
    "${BO3_SDK}/src/graphics/pipeline/shader/shader.cpp"
    "${BO3_SDK}/src/graphics/pipeline/shader/translator.cpp"
    "${BO3_SDK}/src/graphics/pipeline/shader/translator_disasm.cpp")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/present_spirv.h"
    COMMAND "${PYTHON_EXECUTABLE}" "${BO3_ROOT}/scripts/nx/embed-native-shaders.py"
        --dxc "${BO3_ROOT}/tools/dxc-1.9.2609/package/bin/x64/dxc.exe"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/present.hlsl"
        --output "${CMAKE_CURRENT_BINARY_DIR}/present_spirv.h"
    DEPENDS present.hlsl "${BO3_ROOT}/scripts/nx/embed-native-shaders.py" VERBATIM)
target_include_directories(rexgpu-bo3native PRIVATE "${BO3_TRANSLATOR}" "${CMAKE_CURRENT_BINARY_DIR}"
    "${BO3_SDK}/thirdparty/renderdoc" "${BO3_SDK}/thirdparty/crypto")
target_link_libraries(rexgpu-bo3native PRIVATE rexruntime xxHash::xxhash Vulkan::Headers)
target_compile_options(rexgpu-bo3native PRIVATE -ffp-contract=off -fno-strict-aliasing)
target_compile_options(rexgpu-bo3native PRIVATE -gcodeview)
target_link_options(rexgpu-bo3native PRIVATE "-Xlinker" "/DEBUG")
set_target_properties(rexgpu-bo3native PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_executable(bo3_native_memexport_test EXCLUDE_FROM_ALL memexport_test.cpp)
set_target_properties(bo3_native_memexport_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(bo3_native_physical_access_test EXCLUDE_FROM_ALL physical_access_test.cpp)
target_link_libraries(bo3_native_physical_access_test PRIVATE rexruntime)
set_target_properties(bo3_native_physical_access_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_executable(bo3_native_texture_upload_test EXCLUDE_FROM_ALL texture_upload_test.cpp texture_upload.cpp)
target_link_libraries(bo3_native_texture_upload_test PRIVATE rexruntime Vulkan::Headers)
set_target_properties(bo3_native_texture_upload_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_executable(bo3_native_resolve_ownership_test EXCLUDE_FROM_ALL resolve_ownership_test.cpp)
set_target_properties(bo3_native_resolve_ownership_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_executable(bo3_native_color_resolve_test EXCLUDE_FROM_ALL color_resolve_test.cpp color_resolve.cpp)
target_link_libraries(bo3_native_color_resolve_test PRIVATE rexruntime Vulkan::Headers)
set_target_properties(bo3_native_color_resolve_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_executable(bo3_native_pipeline_state_test EXCLUDE_FROM_ALL pipeline_state_test.cpp)
target_link_libraries(bo3_native_pipeline_state_test PRIVATE rexruntime)
set_target_properties(bo3_native_pipeline_state_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_executable(bo3_native_texture_watch_test EXCLUDE_FROM_ALL texture_watch_test.cpp)
target_link_libraries(bo3_native_texture_watch_test PRIVATE rexruntime)
set_target_properties(bo3_native_texture_watch_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
