file(GLOB_RECURSE  ${MAIN_EXEC_NAME}_SRCS "*.cc")

add_executable(${MAIN_EXEC_NAME}
  ${${MAIN_EXEC_NAME}_SRCS}
)

add_subdirectory(enkiTs)
add_subdirectory(libipc)
add_subdirectory(runtime_core)

target_link_libraries(${MAIN_EXEC_NAME}
  PRIVATE ${RUNTIME_IPC}
  PRIVATE ${RUNTIME_SCHED}
  PRIVATE ${RUNTIME_SOURCE}
  ${CONAN_LIBS}
)
