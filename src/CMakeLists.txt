file(GLOB_RECURSE  ${MAIN_EXEC_NAME}_SRCS "*.cc")

add_executable(${MAIN_EXEC_NAME}
  ${${MAIN_EXEC_NAME}_SRCS}
)

add_subdirectory(enkiTs)