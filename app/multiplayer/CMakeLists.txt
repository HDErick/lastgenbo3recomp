# The parent configures the SDK once and provides rexglue_setup_target.
include(generated/default/sources.cmake)
set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES ${GENERATED_SOURCES})
set(REXGLUE_ENTRYPOINT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
set(REXGLUE_HOST_TARGET bo3_mp)

if(WIN32)
    add_executable(bo3_mp WIN32 src/main.cpp ../src/crash_reporter.cpp)
else()
    add_executable(bo3_mp src/main.cpp ../src/crash_reporter.cpp)
endif()
rexglue_setup_target(bo3_mp GPU_PLUGINS xenos)
target_include_directories(bo3_mp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(bo3_mp PRIVATE dbghelp)
target_link_options(bo3_mp PRIVATE "-Xlinker" "/DEBUG")
set_target_properties(bo3_mp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
