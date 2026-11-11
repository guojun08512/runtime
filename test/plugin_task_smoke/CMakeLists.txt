find_package(cJSON REQUIRED)

add_executable(runtime_plugin_task_smoke
    plugin_task_smoke.cc
    ${PROJECT_SOURCE_DIR}/src/runtime_plugin/plugin_manager.cc
    ${PROJECT_SOURCE_DIR}/src/runtime_core/global.cc
    ${PROJECT_SOURCE_DIR}/src/runtime_core/plugin_control.cc
    ${PROJECT_SOURCE_DIR}/src/runtime_core/task.cc
    ${PROJECT_SOURCE_DIR}/src/runtime_core/task_callback.cc
)

target_include_directories(runtime_plugin_task_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(runtime_plugin_task_smoke PRIVATE
    enkiTs
    libipc
    cjson::cjson
    glog::glog
    ${CMAKE_DL_LIBS}
)

add_test(
    NAME runtime_plugin_task_smoke
    COMMAND runtime_plugin_task_smoke
)
