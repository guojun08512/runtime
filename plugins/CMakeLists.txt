option(RUNTIME_BUILD_EXAMPLE_PLUGIN
       "Build the example dynamically loadable I/O plugin" ON)

if(RUNTIME_BUILD_EXAMPLE_PLUGIN)
    add_subdirectory(example_io)
    add_subdirectory(example_counter)

    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/plugins.json" CONTENT
"{
  \"plugins\": [
    {
      \"path\": \"$<TARGET_FILE:runtime_example_io_plugin>\",
      \"auto_start\": true,
      \"config\": {
        \"poll_interval_ms\": 1
      }
    },
    {
      \"path\": \"$<TARGET_FILE:runtime_example_counter_plugin>\",
      \"auto_start\": false,
      \"config\": {}
    }
  ]
}
")
endif()
