find_package(cJSON REQUIRED)

add_executable(runtime_multi_plugin_smoke
    multi_plugin_smoke.cc
    ${PROJECT_SOURCE_DIR}/src/runtime_plugin/plugin_manager.cc
)

target_include_directories(runtime_multi_plugin_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(runtime_multi_plugin_smoke PRIVATE
    cjson::cjson
    glog::glog
    ${CMAKE_DL_LIBS}
)

add_dependencies(runtime_multi_plugin_smoke
    runtime_example_io_plugin
    runtime_example_counter_plugin
)

add_test(
    NAME runtime_multi_plugin_smoke
    COMMAND runtime_multi_plugin_smoke
            $<TARGET_FILE:runtime_example_io_plugin>
            $<TARGET_FILE:runtime_example_counter_plugin>
)
