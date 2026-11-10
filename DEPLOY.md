# ChatServer 部署文档

## 1. 架构

### 1.1 组件职责

| 组件 | 是否必需 | 作用 |
|---|---|---|
| ChatServer | 必需 | 业务服务，对外提供 TCP 长连接（9002）与上传 API（9003） |
| MySQL | 必需 | 用户、好友、群、消息等持久化数据 |
| Redis | 必需 | 在线状态、跨节点消息路由、会话缓存 |
| MinIO（或云 OSS） | 必需 | 图片、文件、视频等媒体存储 |
| 网关 | 视情况 | TLS 终结与流量分发，可由 Nginx 或云 CLB 承担 |

### 1.2 两种部署形态

| 形态 | 适用 | 网关 |
|---|---|---|
| **单机自建** | 开发、演示、小规模 | 本机 Nginx |
| **云上生产** | 正式环境 | Serverless 引导 + 各地域 CLB，业务节点全部私网 |

### 1.3 通用约定

- **必须从仓库根目录启动**：配置文件和 JWT 密钥均按运行时工作目录解析。用 systemd 时务必设置 `WorkingDirectory`。
- **启动即检查依赖**：MySQL / Redis / MinIO 任一不可达，进程直接退出。
- **媒体链路**：下载（GET）和大文件直传（PUT）共用 `/chat/` 路径。上传支持分片断点续传，**下载暂不支持断点续传**（网关已透传 `Range`，客户端后续版本可启用）。

---

## 2. 单机 / 自建部署

### 2.1 编译

```bash
# 系统依赖
sudo apt update
sudo apt install -y build-essential cmake ninja-build git curl zip unzip tar \
    pkg-config autoconf automake autoconf-archive libtool libtool-bin bison flex \
    gperf gettext python3 perl rpcsvc-proto libtirpc-dev

# vcpkg（仅编译机需要，默认 ~/vcpkg）
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# 编译
git clone https://gitee.com/saltedfish_mjc/chat-freely_-server.git ~/ChatServer
cd ~/ChatServer
make deps
make -j$(nproc)
```

> `make deps` 下载失败（国内网络）：
> `export VCPKG_ASSET_SOURCES="x-azurl,https://ghproxy.com/https://github.com"` 后重试。

产物为 `build/ChatServer`，第三方库全部静态链接，可直接拷贝到同架构机器运行。

### 2.2 初始化数据库

```bash
mysql -h <地址> -u <用户名> -p <数据库名> < schema.sql
```

### 2.3 证书与密钥

```bash
# 自签名测试证书（正式环境请换正式证书）
./gen-cert.sh

# JWT 密钥，整个集群只生成一次
./build/ChatServer --gen-jwt-secret ./secret/jwt_secret.key
chmod 600 secret/jwt_secret.key
```

### 2.4 配置

首次运行自动生成 `config/ServerConfig.conf`，按第 5 节修改后重启。

### 2.5 启动（systemd）

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
LimitNOFILE=100000
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

### 2.6 Nginx（单机形态）

```nginx
worker_rlimit_nofile 100000;
events { worker_connections 65535; }

http {
    client_max_body_size 2g;
    server {
        listen 443 ssl;
        ssl_certificate     /etc/nginx/ssl/server.crt;
        ssl_certificate_key /etc/nginx/ssl/server.key;

        location /chat/   { proxy_pass http://127.0.0.1:9000/chat/; proxy_set_header Host $host; proxy_buffering off; }
        location /upload/ { proxy_pass http://127.0.0.1:9003/upload/; proxy_request_buffering off; proxy_read_timeout 300s; }
        location = /config {
            default_type application/json;
            return 200 '{"serverHost":"<你的域名>","tcpPort":9443,"useHttps":true}';
        }
    }
}

stream {
    server {
        listen 9443 ssl;
        ssl_certificate     /etc/nginx/ssl/server.crt;
        ssl_certificate_key /etc/nginx/ssl/server.key;
        proxy_pass 127.0.0.1:9002;
        proxy_timeout 24h;
    }
}
```

---

## 3. 云上生产架构

### 3.1 三层收敛

```
① GET https://api.example.com/config
        → CDN → API 网关 → SCF（按客户端 IP 判定地域）
        ← {"serverHost":"sh.example.com","tcpPort":9443,"useHttps":true}

② 客户端连 sh.example.com:9443
        → 该地域 CLB（TLS 终结 + 健康检查 + 负载均衡）
        → 私网节点 10.0.1.11~13:9002 / 9003

③ 媒体流量：/chat/ → 云 OSS + CDN（不由 CLB 承担）
```

| 层 | 组件 | 公网 | 职责 |
|---|---|---|---|
| 引导 | CDN + API 网关 + SCF | ✅ | 判地域、下发最近入口 |
| 接入 | 每地域 1 个 CLB | ✅ | TLS 终结、节点负载均衡 |
| 计算 | ChatServer 节点 × N | ❌ 私网 | 业务与长连接 |
| 存储 | MySQL / Redis / MinIO | ❌ 私网 | 共享状态 |

**收益**：对外只有引导层和各地域 CLB 两个入口，节点、数据库、存储均不可从公网触达。

### 3.2 引导层：`/config` 下发

SCF 逻辑只需地域映射，不需要维护单机心跳：

```python
REGION_MAP = {"shanghai": "sh.example.com", "guangzhou": "gz.example.com"}

def main(event):
    province = ip2region(event["sourceIp"])       # 离线库，打进函数包
    region   = "shanghai" if province in SH else "guangzhou"
    return {"serverHost": REGION_MAP[region], "tcpPort": 9443, "useHttps": True}
```

**必须给 `/config` 挂 CDN 缓存**（TTL 1~10 分钟）。该接口返回内容静态，缓存后攻击流量在边缘被吸收，既扛打又省函数调用费。

### 3.3 接入层：CLB

- **四层 TCP SSL 监听器**：9443，后端挂本地域各节点 9002
- **七层 HTTPS 监听器**：443，按路径转发 `/upload/` → 9003、`/chat/` → 对象存储内网地址
- 节点挂掉由 CLB 健康检查自动摘除

### 3.4 四项注意

1. **CLB 规格看并发连接数**，不是 QPS。长连接常驻，峰值 3 万在线就需 3 万并发连接，选规格时按此估算并预留余量。
2. **四层空闲超时必须大于客户端心跳间隔**，否则 CLB 静默断开长连接，表现为“莫名掉线”。
3. **后端节点分布在至少两个可用区**，避免单可用区故障导致 CLB 存活但后端全挂。
4. **客户端必须实现回落**：连接入口失败时重新请求 `/config`，由 SCF 导向其他地域，实现自愈。

---

## 4. 多节点部署

### 4.1 原理

所有节点共享同一套 MySQL / Redis / 对象存储，本身无状态：

| 机制 | 实现 |
|---|---|
| 在线状态 | 用户连接时 `SETNX(online_key, node_id, TTL)` 登记所在节点 |
| 跨节点消息 | 查得目标节点后发布到 `channel:node:{node_id}`，目标节点订阅并推送 |
| ID 唯一 | 按 `node_id` 推导雪花 workerId（`node_1→0`、`node_2→1`），跨节点不冲突 |
| 幂等 | 消息表 `idempotent_key` 唯一约束 + `INSERT IGNORE`，重试不重复入库 |

### 4.2 步骤

**① 分发 JWT 密钥（最先做）**

```bash
# 整个集群只生成一次
./build/ChatServer --gen-jwt-secret ./secret/jwt_secret.key

# 分发到其他所有节点
scp secret/jwt_secret.key user@nodeB:~/ChatServer/secret/

# 各节点校验一致（输出应完全相同）
md5sum ~/ChatServer/secret/jwt_secret.key
```

> ⚠️ 必须先分发再启动。新节点若先启动会自行生成不同密钥，之后集群内 token 互验全部失败。

**② 配置 `node_id`**

各节点 `config/ServerConfig.conf` 中 `node_id` 依次设为 `node_1`、`node_2`……（取值 1~32），其余存储配置保持一致。

**③ 入口负载均衡**

云上由地域 CLB 后端挂全部节点；自建用 Nginx：

```nginx
stream {
    upstream chat_tcp {
        least_conn;
        server 10.0.1.11:9002;
        server 10.0.1.12:9002;
    }
    server {
        listen 9443 ssl;
        ssl_certificate     /etc/nginx/ssl/server.crt;
        ssl_certificate_key /etc/nginx/ssl/server.key;
        proxy_pass chat_tcp;
        proxy_timeout 24h;
    }
}
```

### 4.3 验证

```bash
# 各节点频道都在（说明订阅正常）
redis-cli pubsub channels
# 期望：channel:node:node_1、channel:node:node_2

# 各节点启动日志 workerId 不同
# Snowflake init: node_id=node_1, workerId=0
# Snowflake init: node_id=node_2, workerId=1

# 跨节点验收：客户端 A 连 node_1、客户端 B 连 node_2，互发消息能收到；
# 登录后重连到另一节点无需重新登录（证明密钥同步成功）
```

### 4.4 约束

- `node_id` 重复会导致消息路由错乱、在线锁互顶，务必唯一。
- 各节点需 NTP 时间同步，雪花 ID 依赖时钟单调。
- 集群上限 32 节点（workerId 占 5 bit）。

---

## 5. 配置项

`config/ServerConfig.conf`：

| 配置项 | 说明 |
|---|---|
| `node_id` | 节点标识，格式 `node_N`（1~32），多节点时各不相同 |
| `listen_port` / `upload_port` | TCP 长连接 / 上传端口，默认 9002 / 9003 |
| `jwt_secret_file` | 密钥路径，多节点必须指向同一份内容 |
| `mysql_host/port/username/password/dbname` | 数据库连接，云 RDS 填实例地址 |
| `redis_host/port/password` | Redis 连接 |
| `redis_use_ssl` | 云 Redis 强制 TLS 时设 `true` |
| `redis_ssl_cacert` | CA 证书路径，不校验可留空 |
| `redis_connect_timeout` / `redis_socket_timeout` | 单位 ms；走 TLS 或跨地域建议调至 3000~5000 |
| `minio_endpoint` | 服务端访问存储的内网地址 |
| `minio_access_key/secret_key/bucket` | 存储凭据与桶名 |
| `public_url_prefix` | 下载 URL 前缀，客户端可达地址，**必须以 `/` 结尾** |
| `upload_public_host` | 直传 PUT 的目标域名，必须与预签名 URL 实际访问域名一致 |

> **自建 MinIO**：`public_url_prefix` 填网关域名，网关必须转发 `/chat/` 到 MinIO 9000。
> **云 OSS**：`public_url_prefix` 直接填 OSS/CDN 域名，媒体流量旁路网关。

---

## 6. 运维

### 6.1 健康检查

```bash
systemctl status chatserver
ss -tlnp | grep -E '9002|9003'
curl -k https://<域名>/config
curl -k -I https://<域名>/chat/images/xxx.jpg
```

### 6.2 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `make deps` 失败 | 无法访问 GitHub | 设置 `VCPKG_ASSET_SOURCES` 镜像后重试 |
| 启动报依赖不可达 | 地址错误或安全组未放行 | `telnet <host> <port>` 验证，再查密码 |
| Redis 报 TLS 错误 | 云实例强制 TLS | `redis_use_ssl = true` 并配置 CA |
| 9443 连不上 | Nginx 未启动 / stream 块位置错 | stream 块必须在主配置层，不能放进 http{} |
| 上传下载 502 | 后端 9003 / 9000 不可达 | 检查进程与转发目标 |
| 图片无法预览 | `public_url_prefix` 错误 | 必须 `https://<域名>/chat/`，末尾带 `/` |
| 直传报 403 | Host 与签名域名不一致 | `upload_public_host` 与访问域名对齐；网关不得改写 Host |
| 长连接异常掉线 | 入口空闲超时小于心跳 | 调大 CLB 空闲超时或 Nginx `proxy_timeout` |
| 跨节点消息收不到 | `node_id` 重复 | 核对各节点频道与 `node_id` 唯一性 |
| 跨节点 ID 冲突 | 未初始化雪花 workerId | 确认启动日志有 `Snowflake init` |
| 客户端 token 校验失败 | 多节点密钥不一致 | 重新分发同一份密钥并重启 |
