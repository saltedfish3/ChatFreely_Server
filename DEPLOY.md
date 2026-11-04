# ChatServer 部署文档

##  1. 架构与链路

### 1.1 链路总览

| 链路 | 客户端请求 | 服务端组件 | 端口 | 经过Nginx |
|---|---|---|---|---|
| 主服务 | 自定义 TCP，换行分隔 JSON | ChatServer | 9002 | 否 |
| 上传（分片） | `POST /upload/init`、`/upload/part`、`/upload/complete` | UploadWorker | 9003 | 否 |
| 上传（图片） | `PUT /upload/single` | UploadWorker | 9003 | 否 |
| 上传（presigned） | `PUT <presigned URL>`，非图片且 ≤ 100MB | MinIO | 9000 | 否 |
| 下载 / 预览 | `GET <public_url_prefix><key>` | Nginx -> MinIO | 80 -> 9000 | 是 |

### 1.2 关键点

- **上传主链路不经过 Nginx**: 分片上传和图片上传直连 UploadWorker `:9003`; presigned 上传直连 MinIo `:9000`。 
- **下载 / 预览经过 Nginx**: `public_url_prefix` 指向 Nginx `:80`, Nginx反代到 MinIO `:9000`。
- **服务端启动只检查 MySQL、Redis、MinIO**。 Nginx 不是启动依赖，服务端不检查 Nginx。
- **缩略图由服务端生成**: stb 解码 / 缩放/ 解码后写回 MinIO, 不是 MinIO 或 Nginx 做的。

## 2. 依赖组件

### 2.1 运行时核心依赖

| 组件 | 版本要求 | 用途 |
|---|---|---|
| MySQL | 5.7+ / 8.0+ | 用户、好友、群聊、消息持久化 |
| Redis | 5.0+ | 在线状态、pub/sub、缓存、限流 |
| MinIO | RELEASE.2025-09-07 或兼容版本 | 对象存储（图片、视频、文件） |

> 服务端启动时会检查这三项的连通性和认证，任一不可达直接退出。

### 2.2 运行时可选依赖

| 组件 | 版本要求 | 用途 | 不装的影响 |
|---|---|---|---|
| Nginx / Caddy | 任意稳定版 | 下载/预览反代 | 下载不可用，上传不受影响 |

> 也可以不装反代，让客户端直连 MinIO `:9000`，见 §5 模式 A。

### 2.3 编译依赖（系统库）

| 组件 | 头文件 / 库 | 用途 |
|---|---|---|
| libevent2 | `<event2/*>` | 事件循环、`evhttp` 服务端 |
| libcurl | `<curl/curl.h>` | HTTP 客户端（MinIO 签名 GET） |
| hiredis | `<hiredis/hiredis.h>` | Redis 客户端 |
| libmysqlclient | `<mysql/mysql.h>` | MySQL 客户端 |
| libsodium | `<sodium/*>` | 密码哈希、随机数、hex 编解码 |

> 编译前需通过系统包管理器安装，例如 Ubuntu：`apt install libevent-dev libcurl4-openssl-dev libhiredis-dev libmysqlclient-dev libsodium-dev`。

### 2.4 第三方库（源码内置 / Header-only）

这些库放在 `ChatServer/third_party/` 目录下，编译时直接包含，无需系统级安装。

| 库 / 文件 | 用途 |
|---|---|
| `json.hpp` | nlohmann/json，用于解析与序列化通信协议、MinIO API 响应 |
| `snowflake.h` | 雪花算法生成分布式唯一 ID（消息 ID、用户 ID 等） |
| `stb_image.h` | 图片解码（读取原图数据） |
| `stb_image_resize2.h` | 图片缩放（限制长边为 200px） |
| `stb_image_write.h` | 图片编码（生成 jpg / png 缩略图） |

> 另外，`jwt-cpp` 和 `minio-cpp` 建议通过 CMake 的 `FetchContent` 引入，或者放入 `third_party/` 目录，确保编译时能正确寻址。

## 3. 配置说明

默认配置文件 `./config/ServerConfig.conf`，可用 `-c <path>` 指定。

### 3.1 配置模板

```ini
# ========== Server ==========
node_id = node_1
listen_port = 9002
upload_port = 9003
worker_num = 4
upload_worker_num = 4
jwt_secret_file = ./secret/jwt_secret.key

# ========== ThreadPool ==========
thread_pool_size = 8

# ========== Redis ==========
redis_host = 127.0.0.1
redis_port = 6379
redis_password = <redis_password>
redis_pool_size = 4

# ========== MySQL ==========
mysql_host = 127.0.0.1
mysql_port = 3306
mysql_username = <mysql_user>
mysql_password = <mysql_password>
mysql_dbname = <mysql_db>
mysql_min_connection = 3
mysql_max_connection = 10
mysql_min_alive_minutes = 60
mysql_max_alive_minutes = 240
mysql_min_free_minutes = 20
mysql_max_free_minutes = 40

# ========== MinIO ==========
minio_endpoint = http://127.0.0.1:9000
minio_access_key = <minio_access_key>
minio_secret_key = <minio_secret_key>
minio_region = us-east-1
minio_bucket = chat

# ========== Public Access ==========
public_url_prefix = http://<public_host>/chat/
upload_public_host = <public_host>:9003

# ========== Timeouts ==========
mysql_connect_timeout = 3
redis_connect_timeout = 1000
redis_socket_timeout = 1000
http_connect_timeout = 2
http_total_timeout = 5
```

### 3.2 关键项

| 配置项 | 含义 | 正确示例 | 常见错误 |
|---|---|---|---|
| `listen_port` | 主服务 TCP 端口 | `9002` | 与 `upload_port` 冲突 |
| `upload_port` | UploadWorker HTTP 端口 | `9003` | — |
| `jwt_secret_file` | JWT 密钥文件 | `./secret/jwt_secret.key` | 文件不存在，启动失败 |
| `minio_endpoint` | MinIO S3 API 地址 | `http://127.0.0.1:9000` | 写成 `:9001`（Console 端口） |
| `minio_access_key` / `minio_secret_key` | MinIO 凭证 | 与 MinIO 容器 env 一致 | 不一致导致 `InvalidAccessKeyId` |
| `minio_bucket` | bucket 名 | `chat` | 不存在，启动失败 |
| `public_url_prefix` | 下载 URL 前缀 | `http://<host>/chat/` | **末尾少 `/`**、**写成 `127.0.0.1`**、**端口写 `9000` 但实际走 Nginx `80`** |
| `upload_public_host` | 上传入口 host:port | `<host>:9003` | **写成 `127.0.0.1`**，客户端不在本机时连不上 |

**`public_url_prefix` 端口规则：**

- 走 Nginx 反代：`http://<host>/chat/`（端口 80，不写）
- 直连 MinIO：`http://<host>:9000/chat/`
- 走 CDN：`https://<cdn>/chat/`

**必须末尾带 `/`**，代码是纯字符串拼接 `prefix + key`。

`public_url_prefix` 和 `upload_public_host` 的 host 必须是**客户端可达**的 IP 或域名，不能写 `127.0.0.1`（客户端不在服务端本机时）。

### 3.3 JWT 密钥

首次部署生成：

```bash
./ChatServer --gen-jwt-secret ./secret/jwt_secret.key
```

- 文件已存在会报错，不覆盖
- 权限自动 `600`
- 密钥丢失后所有已签发 token 失效，需重新登录

---

## 4. 部署步骤

### 4.1 初始化数据库

首次部署需要创建数据库表。项目根目录下已经提供了 `schema.sql` 文件，直接通过命令行导入即可：

```bash
mysql -h <host> -u <user> -p <db_name> < schema.sql
```

> **注意**：不需要手动插入 `global_seq` 的初始数据。服务端启动时，`SQLPool` 会自动执行 `INSERT IGNORE INTO global_seq` 来确保 `conv_sync` 记录存在。如果日志中出现 `[ERROR] 初始化global_seq失败`，请检查 `global_seq` 表是否已成功创建，或数据库连接权限是否正常。

### 4.2 启动 MinIO

```yaml
# docker-compose.yml
services:
  minio:
    image: minio/minio:RELEASE.2025-09-07T16-13-09Z
    command: server /data --console-address ":9001"
    environment:
      MINIO_ROOT_USER: <accessKey>
      MINIO_ROOT_PASSWORD: <secretKey>
    ports:
      - "9000:9000"
      - "9001:9001"
    volumes:
      - minio-data:/data
    restart: unless-stopped

volumes:
  minio-data:
```

```bash
docker compose up -d minio
docker logs minio --tail 20
```

**注意**：不要设置 `MINIO_ROOT_USER_FILE` / `MINIO_ROOT_PASSWORD_FILE` / `MINIO_ACCESS_KEY_FILE` / `MINIO_SECRET_KEY_FILE`，除非对应文件已挂载到容器内。当前 MinIO 版本会忽略不存在的 `*_FILE` 并回退到直接 env，但升级后可能变为启动失败。

### 4.3 初始化 MinIO bucket

```bash
mc alias set local http://127.0.0.1:9000 <accessKey> <secretKey>
mc mb local/chat
mc anonymous set download local/chat
mc anonymous get local/chat        # 期望输出 download
mc ls local/chat/                  # 应有 images/ videos/ files/
```

> 只想放开三个前缀时：
> ```bash
> mc anonymous set download local/chat/images
> mc anonymous set download local/chat/videos
> mc anonymous set download local/chat/files
> ```

### 4.4 启动 ChatServer

```bash
./ChatServer -c ./config/ServerConfig.conf
```

启动检查顺序：

1. 加载配置
2. MySQL TCP 连通性
3. Redis TCP 连通性
4. MinIO TCP + 凭证 + bucket 存在性（`BucketExists`）
5. `curl_global_init`
6. Redis 连接池 + subscriber
7. 监听 `listen_port`，启动 worker

任一步失败打印 `Start failed: ...` 并退出，不会带病运行。

### 4.5 配置 Nginx

```nginx
# /etc/nginx/conf.d/chatserver.conf
server {
    listen 80;
    server_name <public_host>;

    location /chat/ {
        proxy_pass http://127.0.0.1:9000/chat/;
        proxy_set_header Host $host;
        proxy_set_header Range $http_range;
        proxy_set_header If-Range $http_if_range;
        proxy_buffering off;
        client_max_body_size 2g;
    }
}
```

```bash
nginx -t && systemctl reload nginx
curl -sI http://<public_host>/chat/ | grep -i server   # 期望 Server: nginx/...
```

---

## 5. 三种部署模式

### 模式 A：MinIO 直连（零反代）

```ini
public_url_prefix = http://<minio_host>:9000/chat/
```

```bash
mc anonymous set download local/chat
```

客户端直接访问 `http://<minio_host>:9000/chat/videos/xxx.mp4`。9000 端口需对客户端开放；无 HTTPS（除非 MinIO 配 TLS）；无缓存/限流。

### 模式 B：Nginx / Caddy 反代（推荐）

```ini
public_url_prefix = http://<public_host>/chat/
# HTTPS: public_url_prefix = https://<public_host>/chat/
```

Nginx 配置见 §4.5。Caddy：

```
<public_host> {
    handle_path /chat/* {
        reverse_proxy 127.0.0.1:9000
    }
}
```

### 模式 C：CDN

```ini
public_url_prefix = https://<cdn_domain>/chat/
```

CDN 回源 MinIO `:9000` 或 Nginx `:80`，bucket 匿名读。

---

## 6. 验证清单

### 6.1 启动

```bash
./ChatServer -c ./config/ServerConfig.conf
# 无 "Start failed"，日志出现 "Start Server"
```

### 6.2 小图片上传 + 下载 + 缩略图

```bash
# 1. /upload/init
curl -s -X POST http://<upload_host>:9003/upload/init \
  -H "Authorization: Bearer <token>" \
  -H 'Content-Type: application/json' \
  -d '{"Md5":"<32位md5>","Suffix":"jpg","Size":"<bytes>"}'

# 2. PUT 到返回的 UploadUrl
curl -s -X PUT "<UploadUrl>" \
  -H "Authorization: Bearer <token>" \
  -H 'Content-Type: image/jpeg' \
  --data-binary @test.jpg

# 3. 下载 Url
curl -i "<Url>"              # 200 + image/jpeg

# 4. 缩略图
curl -i "<ThumbnailUrl>"     # 200 + image/jpeg，长边 ≤ 200
```

缩略图边界：
- 原图长边 > 200：生成 `images/<md5>thumbnail.<suffix>`
- 原图长边 ≤ 200：不生成，`ThumbnailUrl` 等于原图 URL
- `skipThumbnail=1`：不生成，响应无 `ThumbnailUrl`
- 缩略图已存在：直接返回，不重复生成

### 6.3 大文件分片 + 断点续传

1. 造 120MB 文件，`/upload/init` 确认 `UploadId`、`PartSize=5242880`、`UploadedParts=[]`
2. 传 5 片后停止
3. 重新 `/upload/init`，确认 `UploadId` 相同、`UploadedParts` 有 1~5
4. 从第 6 片继续，`/upload/complete`
5. `mc stat local/chat/files/<md5>` 大小正确
6. `curl <Url>` 下载，`md5sum` 对比

### 6.4 Range（视频拖动）

```bash
curl -sI -H 'Range: bytes=0-1023' http://<public_host>/chat/videos/<md5>.mp4 \
  | grep -iE 'HTTP|Content-Range'
# 期望 206 Partial Content + Content-Range
```

返回 `200` 说明 Nginx 未透传 `Range`，检查 §4.5 的 `proxy_set_header Range`。

### 6.5 bucket 匿名读

```bash
mc anonymous get local/chat    # 期望 download
```

---

## 7. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `Start failed: MySQL ... unreachable` | host/port 错 | 检查 `mysql_host` / `mysql_port` |
| `Start failed: Redis ... unreachable` | host/port 错 | 检查 `redis_host` / `redis_port` |
| `Start failed: MinIO ... unreachable` | endpoint/port 错 | `minio_endpoint` 应为 `:9000` |
| `Start failed: MinIO check failed ... InvalidAccessKeyId` | accessKey 错 | 对比 MinIO 容器 env 和配置 |
| `Start failed: MinIO check failed ... bucket not exists` | bucket 不存在 | `mc mb local/chat` |
| `Start failed: MinIO endpoint config parse failed` | `minio_endpoint` 格式错 | 确认 `http://host:port`，端口为数字 |
| 上传 401 | token 过期 / Header 格式错 | 重新登录；`Authorization: Bearer <token>` |
| 上传 400 | 缺字段 / 大小非法 / 后缀非法 | 对照 `/upload/init` 校验规则 |
| 上传 413 | 超大小上限 | 图片 10MB、视频 1GB、文件 2GB；单片 ≤ 6MB |
| 上传 500 `PutObject Error` | MinIO 写入失败 | 看日志具体错误，确认凭证/bucket |
| 下载 403 `AccessDenied` | bucket 非匿名读 | `mc anonymous set download local/chat` |
| 下载 404 `NoSuchKey` | key 不对 / 回源 bucket 不对 | `mc ls` 确认 key；检查 `proxy_pass` |
| 下载 404 HTML | Nginx `proxy_pass` 路径错 | 检查 `location /chat/` 和 `proxy_pass .../chat/` |
| 下载 502 | 回源连不上 MinIO | 检查 MinIO 监听地址和 `proxy_pass` 端口 |
| 客户端连 `127.0.0.1:80` 失败 | `public_url_prefix` 写了 `127.0.0.1` | 改成客户端可达 host |
| 视频拖动卡 | Nginx 未透传 `Range` | 补 `proxy_set_header Range` |
| 断点续传失效 | `sendSignedGet` 的 `Host` 头与 MinIO endpoint 不一致 | 检查 `minio_endpoint` 和 `Host` 头 |

---

## 8. 注意事项

1. `public_url_prefix` 末尾必须带 `/`，代码是纯字符串拼接。
2. `upload_public_host` 必须是客户端可达的 IP/域名，不能是 `127.0.0.1`。
3. MinIO 端口是 `9000`（S3 API），不是 `9001`（Console）。
4. bucket 必须匿名读（或改代码走 presigned GET）。
5. Nginx 不是服务端启动依赖，可用性由部署后验证或监控负责。
6. `*_FILE` 变量：当前 MinIO 版本忽略不存在的文件并回退到直接 env，升级前建议删除。
7. 缩略图由服务端同步生成，占用一个 `upload_worker` 线程，图片上传量大时 `upload_worker_num` 建议 ≥ 4。
8. HTTPS：客户端强制 HTTPS 时，在 Nginx/Caddy 终止 TLS，`public_url_prefix` 改 `https://...`；上传链路若也要 HTTPS，需把 `/upload/` 反代到 `:9003` 并重新验证分片上传和断点续传。
9. UploadWorker 的 `evhttp` 不支持 TLS，`:9003` 是明文 HTTP，需 HTTPS 时必须套反代。

---

## 9. 启动顺序

```
1. MySQL
2. Redis
3. MinIO
4. ChatServer
5. Nginx（可选）
```

ChatServer 启动时检查 MySQL、Redis、MinIO，任一不可达直接退出。Nginx 可在 ChatServer 之后启动，不影响上传，只影响下载 URL 是否可访问。
