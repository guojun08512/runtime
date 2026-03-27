cmake_minimum_required(VERSION 3.16)

project( enkiTS
         VERSION "1.11"
         DESCRIPTION "A C and C++ task scheduler for creating parallel programs (with Xenomai support)"
         HOMEPAGE_URL "https://github.com/dougbinks/enkiTS"
         LANGUAGES C CXX)

file(GLOB ENKITS_SRC "*.cc")


# --- 配置选项 ---
option( ENKITS_BUILD_XENOMAI      "Build example applications" OFF )
option( ENKITS_BUILD_EXAMPLES      "Build example applications" OFF )
option( ENKITS_BUILD_SHARED         "Build shared library" ON )
option( ENKITS_INSTALL              "Generate installation target" OFF )
option( ENKITS_SANITIZE             "Build with sanitizers" OFF)

set( ENKITS_TASK_PRIORITIES_NUM "3" CACHE STRING "Number of task priorities, 1-5, 0 for defined by defaults in source" )

# --- 库 Target 创建 ---
if( ENKITS_BUILD_SHARED )
    add_library( enkiTS SHARED ${ENKITS_SRC} )
    target_compile_definitions( enkiTS PRIVATE ENKITS_BUILD_DLL=1 )
    target_compile_definitions( enkiTS INTERFACE ENKITS_DLL=1 )
    set_target_properties( enkiTS PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
else()
    add_library( enkiTS STATIC ${ENKITS_SRC} )
endif()

# --- 基础编译选项 ---
if(ENKITS_SANITIZE)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/INCREMENTAL:NO)
    else()
        add_compile_options(-fsanitize=address -fsanitize-address-use-after-scope -fsanitize=undefined)
        add_link_options(-fsanitize=address -fsanitize-address-use-after-scope -fsanitize=undefined)
    endif()
endif()

# --- Xenomai 与系统相关配置 ---
if( ENKITS_BUILD_XENOMAI )
    # 1. 定义 Xenomai 路径
    set(XENO_SYSROOT "/usr/local")

    # 2. 配置头文件和宏定义 (PUBLIC 确保使用库的可执行文件也能看到)
    target_include_directories( enkiTS PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/enkiTS>
        ${XENO_SYSROOT}/include
        ${XENO_SYSROOT}/include/cobalt
    )
    target_compile_definitions( enkiTS PUBLIC XENOMAI_API_V3 )

    # 3. 线程库查找
    set( CMAKE_THREAD_PREFER_PTHREAD TRUE )
    find_package( Threads REQUIRED )

    # 4. 配置链接选项 (关键：处理 --wrap=main 和 wrappers)
    # 使用 INTERFACE 确保这些选项只传给链接 enkiTS 的可执行文件，而不影响库本身的构建（静态库不链接）
    target_link_options( enkiTS INTERFACE
	"/usr/local/lib/xenomai/bootstrap.o"
        "-L${XENO_SYSROOT}/lib"
        "-L${XENO_SYSROOT}"
        "-Wl,--no-as-needed"
	"-Wl,--wrap=main"
        "-Wl,@${XENO_SYSROOT}/lib/cobalt.wrappers"
        "-Wl,@${XENO_SYSROOT}/lib/modechk.wrappers"
        "-Wl,--dynamic-list=${XENO_SYSROOT}/lib/dynlist.ld"
    )

    # 5. 链接具体的实时库
    target_link_libraries( enkiTS PUBLIC
        ${CMAKE_THREAD_LIBS_INIT}
        cobalt modechk m dl rt pthread
    )

    # 6. C++ 标准要求
    set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fvisibility=hidden -pthread" )

    if( APPLE )
        set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++" )
    endif()
else()
    # 非 UNIX 系统的基础路径
    target_include_directories( enkiTS PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/enkiTS>
    )
endif()

# --- 其他属性配置 ---
if( ENKITS_TASK_PRIORITIES_NUM GREATER "0" )
    target_compile_definitions( enkiTS PUBLIC "ENKITS_TASK_PRIORITIES_NUM=${ENKITS_TASK_PRIORITIES_NUM}" )
endif()

# --- 安装逻辑 ---
if( ENKITS_INSTALL )
    install(TARGETS enkiTS EXPORT enkiTSConfig ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES ${ENKITS_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/enkiTS)
    install(EXPORT enkiTSConfig NAMESPACE enkiTS:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/enkiTS)
endif()