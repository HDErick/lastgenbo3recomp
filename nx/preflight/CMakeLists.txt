cmake_minimum_required(VERSION 3.25)
project(bo3_nx_preflight LANGUAGES C CXX)
if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Use nx/cmake/devkitA64.cmake for the 64-bit Switch probe")
endif()
find_package(Vulkan REQUIRED CONFIG
    PATHS "${BO3_NX_MESA_SDK}/lib/cmake/Vulkan" NO_DEFAULT_PATH)
add_executable(bo3_nx_preflight main.cpp)
target_compile_features(bo3_nx_preflight PRIVATE cxx_std_23)
target_compile_options(bo3_nx_preflight PRIVATE -Wall -Wextra)
target_link_libraries(bo3_nx_preflight PRIVATE Vulkan::Vulkan nx)
target_link_options(bo3_nx_preflight PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/bo3_nx_preflight.map")
find_program(_nacptool nacptool HINTS "${DEVKITPRO}/tools/bin" REQUIRED NO_DEFAULT_PATH)
find_program(_elf2nro elf2nro HINTS "${DEVKITPRO}/tools/bin" REQUIRED NO_DEFAULT_PATH)
add_custom_command(TARGET bo3_nx_preflight POST_BUILD
    COMMAND "${_nacptool}" --create "BO3 NX environment probe" "Wiiucoop" "0.1.0" "${CMAKE_BINARY_DIR}/bo3_nx_preflight.nacp"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:bo3_nx_preflight>" "${CMAKE_BINARY_DIR}/bo3_nx_preflight.stripped.elf"
    COMMAND "${CMAKE_STRIP}" --strip-all "${CMAKE_BINARY_DIR}/bo3_nx_preflight.stripped.elf"
    COMMAND "${_elf2nro}" "${CMAKE_BINARY_DIR}/bo3_nx_preflight.stripped.elf" "${CMAKE_BINARY_DIR}/bo3_nx_preflight.nro"
        "--nacp=${CMAKE_BINARY_DIR}/bo3_nx_preflight.nacp" "--icon=${DEVKITPRO}/libnx/default_icon.jpg"
    VERBATIM)
