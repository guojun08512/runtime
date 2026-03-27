# 使用最新的 Ubuntu 稳定版
FROM ubuntu:24.04

# 避免交互式安装时的提示
ENV DEBIAN_FRONTEND=noninteractive

# 更新系统并安装必要的构建工具和 Python
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    python3 \
    python3-pip \
    python3-venv \
    wget \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# 安装 Conan 2
# 注意：在 Ubuntu 24.04+ 中，建议使用 pipx 或在虚拟环境中安装，
# 但为了容器简单，我们可以使用 --break-system-packages (如果适用)
RUN pip3 install --no-cache-dir "conan>=2.0" --break-system-packages -i https://pypi.tuna.tsinghua.edu.cn/simple 

# 设置工作目录
WORKDIR /home/conan/project

# 初始化 Conan 配置 (生成默认 profile)
RUN conan profile detect

# 默认命令
CMD ["/bin/bash"]