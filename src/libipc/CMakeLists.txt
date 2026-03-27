set (PACKAGE_VERSION 1.4.1)

# --- 配置选项 ---
option( LIBIPC_BUILD_SHARED_LIBS      "Build libipc applications" ON )

message(STATUS "libipc Build shared libs: .")

file(GLOB SRC_FILES "*.*cpp")
aux_source_directory(./sync SRC_FILES)
aux_source_directory(./platform SRC_FILES)
aux_source_directory(./imp SRC_FILES)
aux_source_directory(./mem SRC_FILES)

if (LIBIPC_BUILD_SHARED_LIBS)
  add_library(${RUNTIME_IPC} SHARED ${SRC_FILES})
  target_compile_definitions(${RUNTIME_IPC}
    INTERFACE
      LIBIPC_LIBRARY_SHARED_USING__
    PRIVATE
      LIBIPC_LIBRARY_SHARED_BUILDING__)
else()
  add_library(${RUNTIME_IPC} STATIC ${SRC_FILES})
endif()

# set output directory
set_target_properties(${RUNTIME_IPC}
	PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# set version
set_target_properties(${RUNTIME_IPC}
    PROPERTIES
    VERSION ${PACKAGE_VERSION}
    SOVERSION 3)

# target_include_directories(${RUNTIME_IPC}
#   PUBLIC  
#     "$<BUILD_INTERFACE:./include>"
#     "$<INSTALL_INTERFACE:include>"
#   PRIVATE ./src
#           $<$<BOOL:UNIX>:./platform/linux>)

if(NOT MSVC)
  target_link_libraries(${RUNTIME_IPC} PUBLIC
    $<$<NOT:$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>:pthread>
    $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:rt>)
endif()

install(
  TARGETS ${RUNTIME_IPC}
  EXPORT cpp-ipc-targets
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)

install(EXPORT cpp-ipc-targets
	FILE cpp-ipc-targets.cmake
	NAMESPACE cpp-ipc::
	DESTINATION share/cpp-ipc
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc-config.cmake.in"
[[include(CMakeFindDependencyMacro)
include("${CMAKE_CURRENT_LIST_DIR}/cpp-ipc-targets.cmake")
]])
configure_file("${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc-config.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc-config.cmake" @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc-config.cmake DESTINATION share/cpp-ipc)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
	cppIpcConfigVersion.cmake
	VERSION ${PACKAGE_VERSION}
	COMPATIBILITY AnyNewerVersion
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cppIpcConfigVersion.cmake DESTINATION share/cpp-ipc)