file(GLOB RT_SRCS "*.cc")

find_package(cJSON REQUIRED)
add_library(${RUNTIME_SOURCE} STATIC ${RT_SRCS})

target_include_directories(${RUNTIME_SOURCE} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${RUNTIME_SOURCE} PUBLIC cjson::cjson)