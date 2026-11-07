# ==================== 阶段 1：编译环境 ====================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 安装编译依赖
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libevent-dev \
    libcurl4-openssl-dev \
    libhiredis-dev \
    libmysqlclient-dev \
    libsodium-dev \
    libssl-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# 编译代码
RUN make clean && make

# ==================== 阶段 2：运行环境 ====================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 只安装运行必须的动态链接库
RUN apt-get update && apt-get install -y \
    libevent-2.1-7t64 \
    libevent-openssl-2.1-7t64 \
    libcurl4t64 \
    libhiredis1.1.0 \
    libmysqlclient21 \
    libsodium23 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 拷贝编译好的二进制文件
COPY --from=builder /app/build/ChatServer /app/ChatServer

# 默认启动命令
ENTRYPOINT ["./ChatServer", "-c", "./config/ServerConfig.conf"]
