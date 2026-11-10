# 部署示例文件

本目录提供部署所需的配置模板。按部署形态选择，`<...>` 为需替换的占位符。

| 文件 | 是否必需 | 用途 |
|---|---|---|
| `ServerConfig.conf.example` | 必需 | 复制为 `config/ServerConfig.conf` 后修改 |
| `chatserver.service.example` | 必需 | systemd 服务单元，固定工作目录 |
| `nginx.conf.example` | 单机与地域入口通用 | 单机填本机地址；多节点填各节点内网 IP；用云 CLB 则不需要 |
| `config.json.example` | 可选 | 用 COS 等静态托管下发 `/config` 时使用 |
| `scf-region-dispatch.py.example` | 可选 | 需要按地区就近调度时使用 |

## 使用方式

```bash
cp example/ServerConfig.conf.example  config/ServerConfig.conf
sudo cp example/chatserver.service.example /etc/systemd/system/chatserver.service
```

部署形态与架构说明见仓库根目录 `DEPLOY.md`。
