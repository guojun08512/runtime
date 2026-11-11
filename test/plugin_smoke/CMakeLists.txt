find_package(cJSON REQUIRED)

add_executable(runtime_plugin_smoke
    plugin_smoke.cc
    ${PROJECT_SOURCE_DIR}/src/runtime_plugin/plugin_manager.cc
    ${PROJECT_SOURCE_DIR}/src/runtime_core/task_callback.cc
)

target_include_directories(runtime_plugin_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(runtime_plugin_smoke PRIVATE
    enkiTs
    cjson::cjson
    glog::glog
    ${CMAKE_DL_LIBS}
)

add_dependencies(runtime_plugin_smoke runtime_example_io_plugin)

add_test(
    NAME runtime_plugin_smoke
    COMMAND runtime_plugin_smoke
            $<TARGET_FILE:runtime_example_io_plugin>
)
