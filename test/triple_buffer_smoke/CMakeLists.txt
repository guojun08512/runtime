add_executable(runtime_triple_buffer_smoke
    triple_buffer_smoke.cc
)

target_include_directories(runtime_triple_buffer_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(runtime_triple_buffer_smoke PRIVATE
    libipc
    glog::glog
)

add_test(
    NAME runtime_triple_buffer_smoke
    COMMAND runtime_triple_buffer_smoke
)
