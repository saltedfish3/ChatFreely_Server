
# ChatServer

基于 C++17 开发的高性能即时通信服务端。支持私聊、群聊、大文件断点续传、图片/视频缩略图及全链路 TLS 加密。

## 技术栈

C++17 / libevent / MySQL / Redis / S3 兼容对象存储（MinIO、腾讯 COS、阿里 OSS、AWS S3）/ Nginx / vcpkg + Makefile

## 功能

- 用户系统：注册、登录、JWT 鉴权与 Refresh Token 无感续签

- 社交关系：好友申请/删除、群聊创建/邀请/踢人/解散

- 消息通信：文本发送与同步、全局消息序列（防漏发/重复）

- 媒体文件：小文件直传、大文件分片断点续传、服务端自动生成图片缩略图

- 分布式架构：基于 Redis Pub/Sub 的多节点在线状态同步与消息推送

- 安全通信：HTTP(S) 与 TCP 长连接均采用 TLS 加密

## 环境要求

| 组件 | 版本 |
|---|---|
| 操作系统 | Ubuntu 22.04 / 24.04（x86_64） |
| 编译器 | GCC 11+（C++17） |
| MySQL | 8.0+ |
| Redis | 6.0+ |
| 对象存储 | 任意 S3 兼容服务 |
| Nginx | 需包含 `--with-stream` 模块 |

## 构建

### 1. 安装系统构建工具

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake ninja-build git curl zip unzip tar \
  pkg-config autoconf automake autoconf-archive libtool libtool-bin \
  bison flex gperf gettext python3 perl rpcsvc-proto libtirpc-dev

```

这些只是编译期工具，不会进入运行时产物。

### 2. 安装 vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

```

### 3. 安装依赖

```bash
make deps

```

按 `vcpkg.json` 把依赖安装到仓库内的 `vcpkg_installed/x64-linux/`。

> 网络受限时可先配置代理，或直接拷贝一份已编译好的 `vcpkg_installed/` 目录。

### 4. 编译

```bash
make -j$(nproc)                       # 调试构建
make clean && make MODE=release -j$(nproc)   # 生产构建

```

产物为 `build/ChatServer`，静态链接，部署时单文件即可。

## 运行

```bash

# 1) 首次启动会自动生成默认配置并退出
./build/ChatServer

# 2) 修改配置
vim config/ServerConfig.conf

# 3) 生成 JWT 密钥
mkdir -p secret
./build/ChatServer --gen-jwt-secret ./secret/jwt_secret.key

# 4) 启动
./build/ChatServer

```

> 配置与密钥路径均相对**当前工作目录**，请始终在仓库根目录启动，或用 systemd 的 `WorkingDirectory` 固定。

完整的服务器部署（Nginx、证书、systemd、端口放行）见 [DEPLOY.md](DEPLOY.md) 的 §0 部署总览。

## 目录说明

| 路径 | 说明 | 是否入库 |
|---|---|---|
| `src/` | 源码 | 是 |
| `third_party/` | 头文件型第三方库 | 是 |
| `makefile` / `vcpkg.json` | 构建与依赖清单 | 是 |
| `schema.sql` | 数据库初始化脚本 | 是 |
| `nginx.conf` / `chatserver.conf` | Nginx 配置模板 | 是 |
| `config/ServerConfig.conf` | 运行配置（含密码） | 否 |
| `secret/` | JWT 密钥 | 否 |
| `vcpkg_installed/` | 编译依赖 | 否 |
| `build/` / `logs/` | 构建产物与日志 | 否 |

## 文档

详细的组件准备、Nginx 配置、systemd 服务与故障排查，请参阅 [DEPLOY.md](DEPLOY.md)。

