file(GLOB RT_SRCS "*.cc")

add_library(${RUNTIME_SOURCE} STATIC ${RT_SRCS})

target_include_directories(${RUNTIME_SOURCE} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})