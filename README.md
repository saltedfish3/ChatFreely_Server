# ChatServer

即时通信服务端，支持私聊、群聊、文件上传、断点续传、在线状态同步等。

## 技术栈

C++17 / libevent / MySQL / Redis / MinIO / Nginx / libsodium / jwt-cpp

## 功能

- 用户注册、登陆、JWT鉴权、Refresh Token
- 好友申请、删除、在线状态推送
- 群聊创建、邀请、申请加入、踢人、解散
- 消息发送、同步、断点续传
- 文件上传：小文件 presigned、大文件分片、图片缩略图、视频缩略图
- 多节点：Redis pub/sub 跨节点消息推送

## 快速开始

### 1. 编译

在项目根目录下直接执行 `make` 即可（Makefile 会自动将产物输出到 `build/` 目录）：

```bash
make clean
make
```

### 2. 初始化数据库

```bash
mysql -h <host> -u <user> -p <db_name> < schema.sql
```

### 3. 准备配置与启动

```base
cd build
# 复制配置模板并填入真实的数据库、Redis、MinIO 密码
# 首次启动需生成 JWT 密钥
./ChatServer --gen-jwt-secret ./secret/jwt_secret.key
./ChatServer -c ./config/ServerConfig.conf
```

**详细的部署流程、依赖安装、Nginx 配置和故障排查，请参阅 [DEPLOY.md](DEPLOY.md)。**
