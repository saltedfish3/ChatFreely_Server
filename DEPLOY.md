# ChatServer 部署文档

## 0. 部署总览

从零到可用的完整顺序（细节见对应章节）：

1. 安装系统构建工具（§3）
2. 安装 vcpkg，执行 `make deps`（§3）
3. 编译 `make clean && make MODE=release -j$(nproc)`（§3）
4. 初始化 MySQL 表结构（§2.1）
5. 准备 Redis（§2.2）
6. 创建对象存储桶并配置读写权限（§2.3）
7. 首次启动生成默认配置，编辑 `config/ServerConfig.conf`（§4）
8. 生成 JWT 密钥（§4）
9. 放置 SSL 证书到 `/etc/nginx/ssl/`（§5.3）
10. 配置 Nginx：`conf.d/chatserver.conf` + 主配置 `stream` 块（§5.1、§5.2）
11. 放行防火墙 80 / 443 / 9443
12. 注册 systemd 并启动（§6）
13. 按 §7 逐项验证

## 1. 架构与链路

所有客户端流量经 Nginx 统一入口，内部服务只监听本机端口。

| 链路 | 客户端端口 | Nginx 转发目标 | 加密 |
|---|---|---|---|
| TCP 长连接 | `9443` | `127.0.0.1:9002` | 是（stream 层 TLS 终结） |
| 上传 | `443` | `127.0.0.1:9003` | 是 |
| 下载 / 预览 | `443` | 对象存储域名 | 是 |
| 客户端配置下发 | `443` | Nginx 内部返回 JSON | 是 |

- **启动检查**：ChatServer 启动时会校验 MySQL、Redis、对象存储，任一不可达直接退出。
- **缩略图**：由服务端 `stb` 库生成后写回对象存储。
- **大文件**：非图片且小于阈值的文件通过预签名 URL 直传对象存储，不经过本服务。

## 2. 准备依赖服务

### 2.1 MySQL

```bash
mysql -h <host> -u <user> -p <database> < schema.sql
```

### 2.2 Redis

确保可通过 `redis_host:redis_port` 访问；生产环境建议设置密码。

### 2.3 对象存储

1. 创建桶（如 `chat`），用于存放 `images/`、`videos/` 等对象
2. 授予 ChatServer 读写的 AccessKey / SecretKey
3. 开放公共读（或统一走 Nginx 反代）
4. **若使用腾讯 COS 默认域名**：新桶会强制返回
   `Content-Disposition: attachment`，浏览器与客户端无法内联预览，必须在 Nginx 中用
   `proxy_hide_header` 去掉（见 §5.1）
5. **若开启了防盗链**：务必勾选「允许空 Referer」，否则服务端生成的 URL 会 403
6. 建议为 `uploads/` 前缀配置分片清理生命周期，避免中断的分片长期占用空间

## 3. 编译

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake ninja-build git curl zip unzip tar \
  pkg-config autoconf automake autoconf-archive libtool libtool-bin \
  bison flex gperf gettext python3 perl rpcsvc-proto libtirpc-dev

git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

make deps
make clean && make MODE=release -j$(nproc)
```

产物：`build/ChatServer`（静态链接，无需额外安装运行时库）。

## 4. 应用配置

`config/ServerConfig.conf`，首次启动会自动生成模板。

| 配置项 | 说明 | 示例 |
|---|---|---|
| `listen_port` / `upload_port` | 内部监听端口 | `9002` / `9003` |
| `mysql_host` 等 | 数据库连接 | `127.0.0.1` |
| `redis_host` / `redis_port` | 缓存连接 | `127.0.0.1` / `6379` |
| `minio_endpoint` | 对象存储地址 | 云端：`https://cos.<region>.myqcloud.com`；自建：`http://127.0.0.1:9000` |
| `minio_region` | 区域，**必填** | 云端填实际区域（如 `ap-guangzhou`）；MinIO 填 `us-east-1` |
| `minio_use_virtual_host` | 寻址风格 | 云端 `true`；自建 MinIO 用 IP 时必须 `false` |
| `public_url_prefix` | 文件访问前缀，**必须以 `/` 结尾** | `https://<域名>/chat/` |
| `jwt_secret_file` | JWT 密钥路径 | `./secret/jwt_secret.key` |

> 云的 `virtual_style` 若配成 `false` 会报 `PathStyleDomainForbidden`；
> 自建 MinIO 用 IP 配成 `true` 会报 `HTTP status code 0`。

生成 JWT 密钥：

```bash
mkdir -p secret
./build/ChatServer --gen-jwt-secret ./secret/jwt_secret.key
```

## 5. Nginx 配置

需要两个文件：业务配置放 `conf.d/`，`stream` 块放主配置顶层。

### 5.1 `/etc/nginx/conf.d/chatserver.conf`

见仓库根目录的 `chatserver.conf`，只需替换：

- `<YOUR_DOMAIN>`：你的域名（3 处）
- `<YOUR_STORAGE_HOST>`：对象存储域名（2 处）

关键片段说明：

```nginx
location /chat/ {
    proxy_pass https://<YOUR_STORAGE_HOST>/;
    proxy_ssl_server_name on;
    proxy_set_header Host <YOUR_STORAGE_HOST>;

    proxy_hide_header Content-Disposition;      # 去掉强制下载
    proxy_hide_header x-cos-force-download;

    proxy_set_header Range $http_range;         # 视频拖动 / 断点续传
    proxy_set_header If-Range $http_if_range;
    proxy_buffering off;
}
```

### 5.2 主配置 `stream` 块

`/etc/nginx/nginx.conf` **顶层**（不能放进 `conf.d`，那里是被 `http{}` include 的）：

```nginx
stream {
    server {
        listen 9443 ssl;
        ssl_certificate     /etc/nginx/ssl/server.crt;
        ssl_certificate_key /etc/nginx/ssl/server.key;
        ssl_protocols       TLSv1.2 TLSv1.3;

        proxy_timeout 1h;              # 长连接不要过早断开
        proxy_pass 127.0.0.1:9002;
    }
}
```

### 5.3 SSL 证书放置

证书**不随仓库分发**，需手动放到服务器。

**目录与文件**

```bash
sudo mkdir -p /etc/nginx/ssl
sudo cp server.crt /etc/nginx/ssl/server.crt
sudo cp server.key /etc/nginx/ssl/server.key
sudo chown root:root /etc/nginx/ssl/server.crt /etc/nginx/ssl/server.key
sudo chmod 644 /etc/nginx/ssl/server.crt
sudo chmod 600 /etc/nginx/ssl/server.key
```

| 文件 | 内容 |
|---|---|
| `/etc/nginx/ssl/server.crt` | 证书（正式证书需含中间证书链） |
| `/etc/nginx/ssl/server.key` | 私钥，权限 600 |

**必须同时被两处引用**，漏一处对应链路就会握手失败：

| 位置 | 保护的对象 |
|---|---|
| `conf.d/chatserver.conf` → `server { listen 443 ssl; }` | HTTPS 上传、下载、配置下发 |
| `nginx.conf` 顶层 → `stream { server { listen 9443 ssl; } }` | 客户端 TCP 长连接 |

**证书域名必须与客户端连接的地址一致**

证书的 CN / SAN 必须包含 `<YOUR_DOMAIN>`（或客户端实际使用的 IP）。Qt、浏览器都会校验，
不匹配会直接握手失败。自签名证书还需在客户端信任，或临时跳过校验。

**本地/内网测试可生成自签名证书**

```bash
chmod +x gen-cert.sh
./gen-cert.sh
# 按提示输入服务器域名或 IP，例如 chatfreely.test
# 产物：当前目录下 ssl/server.crt、ssl/server.key（权限已自动设为 644 / 600）
```

再把 `ssl/` 下的两个文件按上面的命令拷到 `/etc/nginx/ssl/`。

> 证书和私钥不要提交到 git。`ssl/` 目录已被 `.gitignore` 忽略。

### 5.4 生效

```bash
nginx -t && systemctl reload nginx
nginx -V 2>&1 | grep -o with-stream   # 必须有输出
```

## 6. 后台运行

`/etc/systemd/system/chatserver.service`：

```ini
[Unit]
Description=ChatServer
After=network.target mysql.service redis.service

[Service]
Type=simple
WorkingDirectory=/home/<user>/ChatServer
ExecStart=/home/<user>/ChatServer/build/ChatServer
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now chatserver
```

`WorkingDirectory` 必须指向仓库根，否则配置和密钥会在其他目录重新生成。

## 7. 验证

```bash
# 配置下发
curl -k https://<域名>/config

# 对象下载（替换为真实对象）
curl -k -I https://<域名>/chat/images/xxx.jpg
# 期望 HTTP 200，且响应头中没有 Content-Disposition

# 上传
curl -k -X POST https://<域名>/upload/single?ObjectKey=test.txt -F "file=@test.txt"

# TCP 长连接：客户端连 9443，服务端日志出现 Client Connect
```

## 8. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| 启动报 `PathStyleDomainForbidden` | 云厂商要求虚拟主机风格 | `minio_use_virtual_host = true` 且 `minio_region` 正确 |
| 启动报 `HTTP status code 0` | endpoint 协议/端口错 | 本地 MinIO 用 `http://127.0.0.1:9000`，`virtual_style = false` |
| 对象 URL 返回 `InvalidRequest` / 缺 Appid | `public_url_prefix` 未包含桶 | 用桶域名或自有域名，且以 `/` 结尾 |
| 返回 `AccessDenied ... referer rule` | 防盗链 | 允许空 Referer |
| 图片/视频只能下载不能预览 | 响应头 `Content-Disposition: attachment` | Nginx 加 `proxy_hide_header`（§5.1） |
| 合并分片报 `InvalidPart` | ETag 引号处理错误 | 确认已使用当前版本代码（ETag 去引号 + 分片排序） |
| `/chat/` 返回 `NoSuchKey <BucketName>chat</BucketName>` | Nginx 上游指到了 MinIO | 改为对象存储域名 |
| 客户端连 9443 失败 | stream 未配置或模块缺失 | 检查 §5.2 与 `nginx -V \| grep with-stream` |
| 客户端连接一段时间后掉线 | `proxy_timeout` 默认 10 分钟 | 设为 `1h` 或客户端定期心跳 |
| 换目录启动后配置「丢失」 | 配置路径相对 CWD | 固定 `WorkingDirectory`，或按需改为可执行文件相对路径 |
