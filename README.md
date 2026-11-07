# ChatServer

基于 C++17 开发的高性能即时通信服务端。支持私聊、群聊、大文件断点续传、图片/视频缩略图及全链路 TLS 加密。

## 技术栈

C++17 / libevent / MySQL / Redis / MinIO / Nginx / Docker

## 功能

- 用户系统：注册、登录、JWT 鉴权与 Refresh Token 无感续签。
- 社交关系：好友申请/删除、群聊创建/邀请/踢人/解散。
- 消息通信：文本发送与同步、全局消息序列（防漏发/重复）。
- 媒体文件：小文件直传、大文件分片断点续传、服务端自动生成图片缩略图。
- 分布式架构：基于 Redis Pub/Sub 的多节点在线状态同步与消息推送。
- 安全通信：HTTP(S) 与 TCP 长连接均采用 TLS 加密。

## 快速开始 (一键部署)

推荐使用 Docker Compose 进行部署，无需配置繁琐的本地环境。

**1. 修改配置**
全局搜索并替换以下文件中的占位符 `<...>`（如 `<你的MySQL密码>`、`<你的公网IP>`）：
- `docker-compose.yml`：填入数据库密码、MinIO 密码等。
- `config/ServerConfig.conf`：填入与上述一致的密码，并将 `redis_host` 等修改为 Docker 服务名。
- `nginx.conf`：修改 `location = /config` 中的下发 IP。

**2. 准备 SSL 证书**
将你的 HTTPS 证书命名为 `server.crt` 和 `server.key`，放入 `ssl/` 目录下。
*(注：如果没有正式证书，请运行 `./gen-cert.sh` 生成自签名测试证书)*

**3. 生成 JWT 密钥**
```bash
docker compose run --rm chatserver ./ChatServer --gen-jwt-secret ./secret/jwt_secret.key
```

**4. 一键启动**
```bash
docker compose up -d --build
```

## 本地开发构建

```bash
# 编译代码 (输出至 build/ 目录)
make clean && make

# 初始化数据库
mysql -h <host> -u <user> -p <db_name> < schema.sql

# 运行服务
cd build
./ChatServer --gen-jwt-secret ./secret/jwt_secret.key
./ChatServer -c ./config/ServerConfig.conf
```

## 文档

**详细的配置参数、端口规划、Nginx 转发规则及故障排查，请参阅 [DEPLOY.md](DEPLOY.md)。**
