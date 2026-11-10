# ChatServer 部署文档

## 1. 架构与链路

ChatServer 采用全链路 TLS 加密设计，所有与客户端的通信均经过网关（Nginx 或云负载均衡）统一解密与转发。

| 链路 | 客户端端口 | 网关转发目标 | 是否加密 |
|---|---|---|---|
| **客户端引导** | `443` (HTTPS) | 静态 JSON（见 2.2，可由任意托管服务下发） | 是 |
| **TCP 长连接** | `9443` (TCP) | 各节点 `9002` | 是（TLS 终结后明文内网转发） |
| **API 上传** | `443` (HTTPS) `/upload/` | 各节点 `9003` | 是（HTTPS） |
| **下载 / 预览 / 大文件直传** | `443` (HTTPS) `/chat/` | MinIO `9000` | 是（HTTPS） |

- **客户端引导机制**：客户端启动时先请求固定的引导地址获取一段静态 JSON
  （`{"serverHost": "<域名>", "tcpPort": 9443, "useHttps": true}`），再据此建立长连接与 API 请求。
  该接口不与后端交互，可由 Nginx 下发，也可由任意静态托管服务下发（见 2.2）。
- **下载 / 直传共用 `/chat/` 路径**：图片/小文件下载（GET）与大文件分片直传（PUT，携带预签名 URL）
  走同一条网关转发规则。这意味着**对象存储选型直接决定网关是否必须存在**（见 2.2）。
- **传输能力现状**：上传方向支持分片断点续传；下载方向暂不支持断点续传
  （网关已透传 `Range` 头、MinIO 原生支持 206 分片响应，客户端后续版本可启用），
  当前大文件下载中断后需整体重新下载。
- **缩略图生成**：由 ChatServer 内置的 stb 库在服务端生成并写回对象存储。
- **服务启动依赖**：启动时检查 MySQL、Redis、MinIO 可达性，任一不可达直接退出。
- **重要**：配置文件与 JWT 密钥均按**运行时工作目录**解析，请始终从仓库根目录启动，
  或用 systemd 的 `WorkingDirectory` 固定（见第 9 节）。

## 2. 部署模式总览

| 模式 | 适用场景 | 说明 |
|---|---|---|
| **全自建单机** | 开发、演示、小规模 | MySQL / Redis / MinIO / Nginx 全部装在同一台机器，Nginx 必须 |
| **云服务混合** | 生产环境推荐 | 存储层换云托管（RDS / 云 Redis / 云 OSS），Nginx 可选甚至可省 |
| **多节点横向扩展** | 高可用 / 大规模 | 2+ 台应用节点共享同一套存储层，网关负载均衡 |

### 2.1 各组件：自建 vs 云服务对照

| 组件 | 自建方式 | 可替换的云服务 | 迁移要点 |
|---|---|---|---|
| **MySQL** | 同机/独立机安装 MySQL | 阿里云 RDS、腾讯云 TDSQL-C、AWS RDS 等 | 把 `mysql_host/port/username/password/dbname` 指向实例即可；用 `schema.sql` 初始化一次表结构 |
| **Redis** | 同机/独立机安装 Redis | 阿里云 Tair、腾讯云 Redis、AWS ElastiCache 等 | 指向实例地址并填密码；**若实例强制 TLS**，设 `redis_use_ssl = true` 并按需配置 `redis_ssl_cacert` |
| **MinIO** | 安装 MinIO Server | 云托管 MinIO；其他 S3 兼容存储（COS/OSS/S3 兼容端点） | 见 2.2：自建 MinIO 时网关必须；云 OSS 时下载/直传可直接指向 CDN 域名。SDK 走 S3 兼容协议，第三方 S3 兼容存储理论可行但未逐一验证，生产建议自建 MinIO 或云托管 MinIO |
| **Nginx** | 宿主机安装 nginx | 云 LB、API 网关、CDN 等，甚至可整体省略 | **是否必须取决于对象存储选型**，逐项替代方案见 **2.2** |

### 2.2 网关（Nginx）是否必须：由对象存储选型决定

Nginx 在本架构中承担 4 个独立职责，其中「下载/直传转发」这一项是否需要，
**完全取决于你用自建 MinIO 还是云 OSS**：

**情况 A：自建 MinIO（没有云 OSS）→ 网关必须**

客户端所有媒体流量都是 HTTPS，而 MinIO 只监听明文 9000 端口，中间必须有 TLS 网关：

- 下载/预览：客户端 GET `https://<域名>/chat/...` → 网关 → MinIO 9000
- 大文件直传：客户端 PUT 预签名 URL（同为 `/chat/` 路径）→ 网关 → MinIO 9000

两条流量均无法绕开网关。这是自建 MinIO 部署的默认形态，
由于下载方向没有断点续传需求（见第 1 节），经网关一次性下载对体验影响可控。

**情况 B：云 OSS（含 CDN 加速）→ 网关可选，媒体流量可整体旁路**

把对象存储相关的两个配置指向云 OSS/CDN 域名即可：

- `public_url_prefix = https://<OSS或CDN域名>/chat/`（下载流量直接走 CDN，不经网关）
- 直传 PUT 同样指向 OSS 域名（由预签名 URL host 决定）

此时网关只剩「TCP 9443 长连接」和「/upload/ API」两项职责，若这两项也交给云 LB，
Nginx 可以完全不用。

**各职责与云服务对照**（无论哪种情况，引导配置始终是静态 JSON，任意托管均可）：

| # | Nginx 承担的职责 | 云服务替代方案 |
|---|---|---|
| 1 | `/config` 引导配置下发 | **任意静态文件托管**：对象存储静态网站（OSS/COS/S3）+ CDN、云函数（SCF/Lambda）、API 网关固定响应 |
| 2 | `443` `/chat/` → MinIO（下载 GET + 直传 PUT） | 情况 A 用七层云 LB 转发；情况 B 直接旁路到 CDN/OSS 域名，网关不再承担此项 |
| 3 | `443` `/upload/` → 节点 9003 | 七层云 LB 按路径转发、API 网关 |
| 4 | `9443` TCP TLS 长连接 | 四层云 LB 的 TCP/TLS 监听器（TCPSSL），后端挂各节点 9002。**监听器空闲超时要大于客户端心跳间隔** |
| 5 | `80 → 443` 跳转、TLS 证书 | 云 SSL 证书服务（免费证书 + 自动续期），LB 监听器直接挂证书 |

**与多节点部署的关系**：引导下发的 `serverHost` 填网关（LB/域名）而非某个节点的 IP，
客户端不需要知道节点列表——由 LB 决定连到哪台节点，跨节点消息由 Redis 路由。
因此多节点扩缩容时，引导配置永远不用改。

>   无论 `/config` 由谁下发（Nginx 还是静态托管），JSON 里的 `serverHost`、`tcpPort`
> 必须与实际网关监听器保持同步；对象存储选型切换时（A ↔ B），同步更新
> `public_url_prefix` 与 `upload_public_host` 并重启服务端。

## 3. 编译安装（应用节点）

每台应用节点都需要编译（或编译一次后分发二进制，静态链接可直接拷贝 `build/ChatServer`）。

```bash
# 3.1 系统依赖
sudo apt update
sudo apt install -y build-essential cmake ninja-build git curl zip unzip tar \
    pkg-config autoconf automake autoconf-archive libtool libtool-bin bison flex \
    gperf gettext python3 perl rpcsvc-proto libtirpc-dev

# 3.2 vcpkg（仅编译机需要；VCPKG_ROOT 默认 ~/vcpkg）
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# 3.3 编译
cd ~/Desktop/ChatServer
make deps
make -j$(nproc)
```

> 国内网络 `make deps` 下载失败时：
> `export VCPKG_ASSET_SOURCES="x-azurl,https://ghproxy.com/https://github.com"` 后重试。

## 4. 数据库初始化（自建 MySQL 与云 RDS 通用）

```bash
mysql -h <MySQL地址> -u <用户名> -p <数据库名> < schema.sql
```

## 5. TLS 证书

将正式证书命名为 `server.crt` / `server.key`，供 Nginx（或云 LB 证书控制台）使用。

没有正式证书时，可在仓库根目录运行 `./gen-cert.sh` 生成自签名测试证书（仅测试用，客户端需关闭证书校验）。

## 6. JWT 密钥

```bash
./build/ChatServer --gen-jwt-secret ./secret/jwt_secret.key
```

**多节点部署时，所有节点必须使用同一份密钥文件**（见第 8 节），先分发再启动。

## 7. 核心配置项说明

`config/ServerConfig.conf` 首次运行自动生成，关键项：

| 配置项 | 含义 | 说明 |
|---|---|---|
| `node_id` | 节点唯一标识 | 格式必须为 `node_N`（N 取 1~32），**多节点时各不相同**；内部据此推导雪花算法 workerId，保证跨节点 ID 不冲突 |
| `listen_port` / `upload_port` | TCP 长连接 / HTTP 上传端口 | 保持 `9002` / `9003` 不变 |
| `mysql_*` | 数据库连接 | 自建填内网 IP，云 RDS 填实例连接地址 |
| `redis_*` | Redis 连接 | 云 Redis 强制 TLS 时设 `redis_use_ssl = true`，`redis_ssl_cacert` 填 CA 证书路径（不校验可留空） |
| `redis_connect_timeout` / `redis_socket_timeout` | Redis 超时（ms） | Redis 跨地域/走 TLS 时建议调大到 3000~5000 |
| `minio_endpoint` | 对象存储 API 地址 | 服务端内网访问地址，如 `http://10.0.0.5:9000` 或云 OSS 内网 endpoint |
| `public_url_prefix` | 下载 URL 前缀 | **情况 A（自建 MinIO）**：`https://<网关域名>/chat/`；**情况 B（云 OSS）**：`https://<OSS或CDN域名>/chat/`（**必须以 `/` 结尾**） |
| `upload_public_host` | 直传对外地址 | 客户端直传 PUT 的目标 host：情况 A 填网关域名；情况 B 填 OSS bucket/CDN 域名。必须与预签名 URL 实际访问域名一致，否则直传报签名错误 |

## 8. 多节点部署

### 8.1 原理

所有节点共享同一套 MySQL / Redis / 对象存储：

- **在线状态**：用户连接时以 `SETNX(online_key, node_id, TTL)` 在 Redis 上登记所在节点。
- **跨节点消息**：节点从 Redis 查到目标用户所在节点后，发布到 `channel:node:{node_id}`，目标节点订阅频道并推送给本机连接。
- **ID 唯一性**：各节点按 `node_id` 推导雪花 workerId（`node_1→0`、`node_2→1`……），跨节点生成的消息 ID 永不冲突。
- **幂等保障**：消息表对 `idempotent_key` 有唯一约束 + `INSERT IGNORE`，客户端重试落到任意节点都不会重复入库。

### 8.2 部署步骤

1. **分发 JWT 密钥（必须最先做）**

   所有节点必须持有**同一份** `secret/jwt_secret.key`，否则 A 节点签发的 token 到 B 节点验签失败，
   用户会被反复登出。在第一台节点上生成一次，然后分发到所有其他节点：

   ```bash
   # 在第一台节点上生成（整个集群只执行一次）
   cd ~/Desktop/ChatServer
   ./build/ChatServer --gen-jwt-secret ./secret/jwt_secret.key
   chmod 600 secret/jwt_secret.key

   # 分发到其余所有节点（nodeB/nodeC 替换为实际地址，路径保持一致）
   scp secret/jwt_secret.key user@nodeB:~/Desktop/ChatServer/secret/
   scp secret/jwt_secret.key user@nodeC:~/Desktop/ChatServer/secret/

   # 校验各节点密钥一致（所有节点执行，输出应完全相同）
   md5sum ~/Desktop/ChatServer/secret/jwt_secret.key
   ```

   >   顺序很重要：**先分发、再启动**。新节点在没有密钥文件时就启动，
   > 会生成一把属于自己的新密钥——之后必须删掉重新分发，否则集群内 token 互验全部失败。

2. **配置各节点 `config/ServerConfig.conf`**：`node_id` 分别设为 `node_1`、`node_2`……（1~32），其余存储相关配置保持一致。

3. **网关负载均衡**（自建 Nginx 时，网关机上的 nginx.conf）：

   ```nginx
   stream {
       upstream chat_tcp {
           server 10.0.0.11:9002;
           server 10.0.0.12:9002;
       }
       server {
           listen 9443 ssl;
           ssl_certificate     /etc/nginx/ssl/server.crt;
           ssl_certificate_key /etc/nginx/ssl/server.key;
           ssl_protocols TLSv1.2 TLSv1.3;
           proxy_pass chat_tcp;
       }
   }
   ```

   HTTP 443 的 `/upload/`、`/chat/` 分流同理，把 `proxy_pass` 指向多台上游即可。
   使用云 LB 时，在四层/TCPSSL 监听器后端挂多节点 9002 即可。

4. **验证**：

   ```bash
   redis-cli pubsub channels
   # 应同时看到 channel:node:node_1 与 channel:node:node_2

   # 各节点启动日志应出现（workerId 各不相同）：
   # Snowflake init: node_id=node_1, workerId=0
   ```

   然后用两个客户端分别连到不同节点，互发消息验证跨节点路由（登录一次后，
   客户端重连到另一台节点不被要求重新登录，即密钥同步成功）。

### 8.3 注意事项

- `node_id` 一旦重复会导致消息路由错乱与在线锁互顶，务必唯一。
- 密钥分发后各节点文件权限建议 600，且路径必须与各节点配置的 `jwt_secret_file` 一致。
- 扩容上限为 32 个节点（雪花 workerId 5 bit）；如需更多，改用不同 datacenterid 组合。

## 9. systemd 托管（推荐）

配置与密钥按运行目录解析，必须用 `WorkingDirectory` 固定为仓库根目录：

```ini
# /etc/systemd/system/chatserver.service
[Unit]
Description=ChatServer
After=network.target

[Service]
Type=simple
User=chat
WorkingDirectory=/opt/ChatServer
ExecStart=/opt/ChatServer/build/ChatServer
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now chatserver
journalctl -u chatserver -f
```

## 10. 服务验证

```bash
# 进程与端口
systemctl status chatserver
ss -tlnp | grep -E '9002|9003'

# 引导配置下发
curl -k https://<你的域名>/config

# 媒体下载（经网关或 CDN，视对象存储选型而定）
curl -k -I https://<你的域名>/chat/images/xxxx.jpg

# TCP 长连接：客户端连接 9443，服务端日志出现 Client Connect 即链路正常
```

## 11. 故障排查

| 现象 | 可能原因 | 处理方式 |
|---|---|---|
| `make deps` 下载失败 | 网络无法访问 GitHub | `export VCPKG_ASSET_SOURCES="x-azurl,https://ghproxy.com/https://github.com"` 后重试 |
| 启动报 MySQL/Redis/MinIO unreachable | 地址或安全组不通 | 先 `telnet <host> <port>` 验证连通性，再查密码 |
| Redis 连接报 TLS 相关错误 | 云实例强制 TLS 但未开启客户端 SSL | `redis_use_ssl = true`，按云厂商文档配置 CA 证书 |
| 客户端连 9443 被拒 | Nginx stream 块未生效 | stream 块必须位于 nginx.conf 主配置层（不能放进 http{}），`nginx -t` 检查 |
| 上传/下载 502 | 后端节点 9003 / MinIO 9000 不可达 | 检查进程存活与网关转发目标 |
| 图片上传成功但无法预览 | `public_url_prefix` 配置错 | 必须为 `https://<域名>/chat/` 且以 `/` 结尾 |
| 直传 PUT 报 403 / SignatureDoesNotMatch | `upload_public_host` 与实际访问域名不一致 | 二者保持一致；网关转发 `/chat/` 时不得改写 Host 头（`proxy_set_header Host $host`） |
| 下载 404 或签名过期 | 预签名 URL 超过有效期 | 重新获取，检查客户端与服务器时间同步 |
| 大文件直传失败 | 网关请求体限制或缓冲 | `client_max_body_size` 调大（示例为 2g）；`/chat/` 建议加 `proxy_request_buffering off` 流式转发 |
| 多节点互发消息收不到 | `node_id` 重复或未订阅 | `redis-cli pubsub channels` 核对各节点频道；确认 node_id 唯一 |
| 跨节点消息 ID 冲突 | 启动日志未见 Snowflake init | 确认代码含 `sfGen.init` 逻辑，node_id 格式为 `node_N` |
| 客户端报 token 校验失败 | 多节点 JWT 密钥不一致 | 重新分发同一份 `jwt_secret.key` 并重启所有节点 |
