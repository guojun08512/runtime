add_library(runtime_example_counter_plugin MODULE
    example_counter_plugin.cc
)

target_include_directories(runtime_example_counter_plugin PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(runtime_example_counter_plugin PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

if(APPLE)
    target_link_options(runtime_example_counter_plugin PRIVATE
        "-Wl,-undefined,error"
    )
elseif(UNIX)
    target_link_options(runtime_example_counter_plugin PRIVATE
        "-Wl,-z,defs"
    )
endif()
