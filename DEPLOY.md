# ChatServer 部署文档

## 1. 架构与链路

ChatServer 采用全链路 TLS 加密设计，所有与客户端的通信均经过 Nginx 统一网关进行解密与转发。

| 链路 | 客户端请求端口 | Nginx 转发目标 (内部网络) | 是否加密 |
|---|---|---|---|
| **TCP 长连接** | `9443` (TCP) | `chatserver:9002` | 是 (TLS 终结) |
| **API 上传** | `443` (HTTPS) | `chatserver:9003` | 是 (HTTPS) |
| **下载 / 预览** | `443` (HTTPS) | `minio:9000` | 是 (HTTPS) |

- **服务启动依赖**：ChatServer 启动时会检查 MySQL、Redis、MinIO。任一不可达直接退出。
- **缩略图生成**：由 ChatServer 内置的 `stb` 库在服务端生成并写回 MinIO。
- **大文件直传**：非图片且 < 100MB 的文件，由 MinIO 生成预签名 URL，客户端直接 PUT 到 MinIO（受 Nginx 保护）。

## 2. 快速部署（Docker Compose）

推荐使用 Docker Compose 部署，无需手动安装任何编译环境。

### 2.1 准备工作
解压交付包后，使用文本编辑器全局搜索并替换 `<...>` 占位符：
1. **`docker-compose.yml`**：
   - 填入 MySQL, Redis, MinIO 的密码。
   - 将 `MINIO_SERVER_URL` 修改为你的公网 IP 或域名（如 `https://1.2.3.4`）。
2. **`config/ServerConfig.conf`**：
   - 填入与上述一致的密码。
   - 将 `public_url_prefix` 修改为 `https://<你的公网IP>/chat/`。
3. **`nginx.conf`**：
   - 搜索 `location = /config`，将里面的 IP 替换为你的公网 IP 或域名。

### 2.2 准备 SSL 证书
将你的正式证书命名为 `server.crt` 和 `server.key`，放入根目录的 `ssl/` 文件夹中。
> 如果没有正式证书，可在根目录运行 `./gen-cert.sh` 生成自签名测试证书。

### 2.3 生成 JWT 密钥
首次部署需要生成用于签发 Token 的 JWT 密钥：

```bash
docker compose run --rm chatserver ./ChatServer --gen-jwt-secret ./secret/jwt_secret.key
```

### 2.4 一键启动
```bash
docker compose up -d --build
```
*启动后，Docker 会自动初始化 MySQL 数据表、创建 MinIO 等。*

## 3. 核心配置项说明

如果你的环境需要特殊调整，请参考以下关键配置：

| 配置项 (ServerConfig.conf) | 含义 | 推荐配置 |
|---|---|---|
| `listen_port` / `upload_port` | 内部通信端口 | 保持 `9002` / `9003` 不变 |
| `mysql_host` / `redis_host` | 数据库/缓存地址 | Docker 内部必须填服务名：`mysql` / `redis` |
| `minio_endpoint` | MinIO API 地址 | Docker 内部填：`http://minio:9000` |
| `public_url_prefix` | 下载 URL 前缀 | `https://<公网IP>/chat/`（**必须以 `/` 结尾**） |

## 4. 服务验证

### 4.1 检查容器状态
```bash
docker ps
```
确保 `chatserver-core`, `chatserver-mysql`, `chatserver-redis`, `chatserver-minio`, `chatserver-nginx` 全部处于 `Up` 状态。

### 4.2 验证 HTTPS 链路
```bash
# 验证配置下发接口
curl -k https://<公网IP>/config

# 验证 MinIO 对象下载 (替换为实际存在的文件名)
curl -k -I https://<公网IP>/chat/images/xxxx.jpg
# 期望返回 HTTP 200 与 Server: nginx
```

### 4.3 验证 TCP 加密长连接
使用你的客户端连接 `9443` 端口。服务端日志若出现 `Client Connect`，说明 Nginx 成功将加密流量透传给了内部服务。

## 5. 故障排查

| 现象 | 可能原因 | 处理方式 |
|---|---|---|
| 客户端连接 `9443` 被拒 | Nginx 未启动或证书路径错 | 检查 `docker logs chatserver-nginx` 报错 |
| 上传或下载报 `502 Bad Gateway` | 内部容器未启动或崩溃 | 检查 `docker logs chatserver-core` |
| 图片上传成功但无法预览 | `public_url_prefix` 配置错 | 确保 URL 前缀是 `https://IP/chat/` 且带斜杠 |
| 启动报 `AccessDenied` | MinIO 密码不匹配 | 检查 `docker-compose.yml` 与 `ServerConfig.conf` 的密码 |
| 客户端报 `TLS initialization failed`| Windows 缺少 OpenSSL 库 | 将 `libssl-3-x64.dll` 等放入客户端同级目录 |
