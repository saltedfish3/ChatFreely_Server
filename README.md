# ChatServer

基于 C++17 开发的高性能即时通信服务端。支持私聊、群聊、大文件断点续传、图片/视频缩略图及全链路 TLS 加密。

## 技术栈

C++17 / libevent / MySQL / Redis / MinIO / Nginx / vcpkg（清单模式依赖管理）

## 功能

- 用户系统：注册、登录、JWT 鉴权与 Refresh Token 无感续签。
- 社交关系：好友申请/删除、群聊创建/邀请/踢人/解散。
- 消息通信：文本发送与同步、全局消息序列（防漏发/重复）、基于幂等键的可靠投递。
- 媒体文件：小文件直传、大文件分片断点续传、服务端自动生成图片缩略图。
- 分布式架构：基于 Redis Pub/Sub 的多节点在线状态同步与跨节点消息推送，支持水平扩展。
- 安全通信：HTTP(S) 与 TCP 长连接均采用 TLS 加密；Redis 支持可选 TLS 连接。

## 架构特点

四大外部依赖（MySQL / Redis / MinIO / Nginx）均**不与本服务绑定**：

- 全部支持替换为云厂商托管服务（RDS、云 Redis、S3 兼容对象存储、云负载均衡）。
- 支持多节点部署：各节点无状态（状态全部收敛到 MySQL / Redis / 对象存储），通过 Nginx 或云 LB 做负载均衡即可横向扩容。

详见 [DEPLOY.md](DEPLOY.md) 的「部署模式总览」与「多节点部署」章节。

## 快速开始（源码构建）

### 1. 环境准备（Ubuntu/Debian）

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git curl zip unzip tar \
    pkg-config autoconf automake autoconf-archive libtool libtool-bin bison flex \
    gperf gettext python3 perl rpcsvc-proto libtirpc-dev
```

### 2. 安装 vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
```

### 3. 拉取并编译

```bash
git clone https://gitee.com/saltedfish_mjc/chat-freely_-server.git ~/Desktop/ChatServer
cd ~/Desktop/ChatServer

make deps     # 按 vcpkg.json 清单安装依赖至 vcpkg_installed/（静态链接）
make -j$(nproc)
```

编译产物为 `build/ChatServer`，所有第三方库均为静态链接，部署机无需安装任何 vcpkg 依赖。

### 4. 初始化数据库

```bash
mysql -h <MySQL地址> -u <用户名> -p <数据库名> < schema.sql
```

### 5. 生成 JWT 密钥并首次运行

```bash
./build/ChatServer --gen-jwt-secret ./secret/jwt_secret.key

# 从仓库根目录启动（配置与密钥路径均相对运行目录解析）
./build/ChatServer
```

首次运行会自动生成 `config/ServerConfig.conf` 默认配置，按需修改后重启。配置项说明见 [DEPLOY.md](DEPLOY.md)。

## 文档

**详细的部署模式（自建 / 云服务混合 / 多节点）、Nginx 配置、systemd 托管及故障排查，请参阅 [DEPLOY.md](DEPLOY.md)。**
