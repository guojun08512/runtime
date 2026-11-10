cmake_minimum_required(VERSION 3.16)

# add_subdirectory(enkiTs)

add_executable(ipc_task_sender ipc_task_sender.cpp)
# libipc target 已在顶层 CMake 中定义 (add_subdirectory(libipc))
target_link_libraries(ipc_task_sender PRIVATE libipc glog::glog)

add_executable(plugin_task_sender plugin_task_sender.cpp)
target_link_libraries(plugin_task_sender PRIVATE libipc glog::glog)

add_executable(plugin_hook_sender plugin_hook_sender.cpp)
target_link_libraries(plugin_hook_sender PRIVATE libipc glog::glog)
