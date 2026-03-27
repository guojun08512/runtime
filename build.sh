#!/bin/bash
set -e

# 默认参数
arch=$(uname -m) # 自动检测当前架构
type=Release
source_build=0
export_pkg=0

# 映射架构名称到 Conan Profile
if [ "$arch" = "x86_64" ]; then arch="x64"; fi

function do_build() {
    echo -e "\033[1;32mStarting Conan 2.0 Build Flow...\033[0m"
    
    # 1. 安装依赖并生成工具链 (--output-folder 会定义所有后续路径的基准)
    # 产物将位于 ./build/ 目录下
    local build_policy="missing"
    if [ ${source_build} = 1 ]; then build_policy="*"; fi

    conan install . \
        -pr:b=default \
        -pr:h=${arch} \
        -s build_type=${type} \
        --build=${build_policy} \
        --output-folder=build

    # 2. 编译项目
    # Conan 会自动寻找 build/Release/generators 里的工具链
    conan build . \
        -pr:h=${arch} \
        -s build_type=${type} \
        --output-folder=build

    # 3. 如果需要导出到本地缓存
    if [ ${export_pkg} = 1 ]; then
        conan export-pkg . -pr:h=${arch} -s build_type=${type} --output-folder=build -f
    fi

    echo -e "\033[32mBuild complete! Check the './build/${type}' directory.\033[0m"
}

# 简单的参数解析
while getopts "t:b:cse" opt; do
  case $opt in
    t) arch=$OPTARG ;;
    b) type=$OPTARG ;;
    c) rm -rf build && echo "Cleaned." && exit 0 ;;
    s) source_build=1 ;;
    e) export_pkg=1 ;;
    *) echo "Usage: ./build.sh [-t arch] [-b type] [-c] [-s] [-e]"; exit 1 ;;
  esac
done

do_build