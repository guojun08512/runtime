#!/bin/bash
set -e

# 默认参数
arch=x64
type=Release
source_build=0
export_package=0

# 颜色定义
BOLDGREEN='\033[1;32m'
ENDCOLOR='\033[0m'

function do_build() {
    echo -e "${BOLDGREEN}Conan 2.0 Build Option:${ENDCOLOR}"
    echo -e "  Arch[${arch}] Type[${type}] SourceBuild[${source_build}]"

    # 1. 执行 conan install
    # --build=missing: 缺少二进制时编译
    # --build=*: 全量源码编译 (对应原脚本 -s 参数)
    local build_policy="missing"
    if [ ${source_build} = 1 ]; then
        build_policy="*"
    fi

    echo "Running: conan install ..."
    conan install . \
        -pr:b=default \
        -pr:h=${arch} \
        -s build_type=${type} \
        --build=${build_policy} \
        --output-folder=build

    # 2. 执行 conan build
    # Conan 2.0 会自动根据 layout 找到 build 目录
    echo "Running: conan build ..."
    conan build . \
        -pr:h=${arch} \
        -s build_type=${type} \
        --output-folder=build

    # 3. 导出包到本地缓存 (对应原脚本 -e 参数)
    if [ ${export_package} = 1 ]; then
        echo "Running: conan export-pkg ..."
        conan export-pkg . \
            -pr:h=${arch} \
            -s build_type=${type} \
            --output-folder=build \
            -f
    fi

    echo -e "\033[32m ========================================================== \033[0m"
    echo -e "\033[32m Build ${arch}_${type} complete using Conan 2.0 :) \033[0m"
    echo -e "\033[32m ========================================================== \033[0m"
}

usage() {
  echo "Usage: $0 [-t <x64|aarch64>] [-b <debug|release>] [-c] [-s] [-e]"
  echo "  -t  Target platform (x64, aarch64)"
  echo "  -b  Build type (Debug, Release)"
  echo "  -c  Clean build directory"
  echo "  -s  Build everything from source"
  echo "  -e  Export package to conan local cache"
  exit 1
}

# 解析参数
while getopts ":t:b:hcse" opt; do
  case $opt in
    t) arch=$OPTARG ;;
    b) type=$OPTARG ;;
    h) usage ;;
    c) 
      rm -rf build
      echo "Cleaned up ./build folder."
      exit 0 
      ;;
    e) export_package=1 ;;
    s) source_build=1 ;;
    *) usage ;;
  esac
done

# 执行构建
do_build