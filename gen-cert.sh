#!/bin/bash

# ==========================================
# 一键生成自签名证书脚本 (用于内网/本地测试)
# ==========================================

echo "=========================================="
echo "     ChatServer 自签名证书生成工具"
echo "=========================================="
echo ""

# 检查 openssl 是否安装
if ! command -v openssl &> /dev/null; then
    echo "❌ 错误: 未检测到 openssl，请先安装 (Ubuntu: sudo apt install openssl)"
    exit 1
fi

read -p "请输入你的服务器 IP 或域名 (例如 192.168.1.100 或 api.example.com): " SERVER_HOST

if [ -z "$SERVER_HOST" ]; then
    echo "❌ 错误: 输入不能为空！"
    exit 1
fi

# 创建 ssl 目录
mkdir -p ssl

# 生成临时配置文件以支持 SAN (Subject Alternative Name)
# 现代浏览器和客户端(如 Qt)强制要求证书包含 SAN 字段
TEMP_CNF=$(mktemp)
cat > "$TEMP_CNF" <<EOF
[req]
distinguished_name = req_distinguished_name
x509_extensions = v3_req
prompt = no

[req_distinguished_name]
C = CN
ST = State
L = City
O = Organization
CN = ${SERVER_HOST}

[v3_req]
subjectAltName = @alt_names

[alt_names]
DNS.1 = ${SERVER_HOST}
IP.1 = ${SERVER_HOST}
EOF

echo ""
echo "⏳ 正在生成证书，请稍候..."

# 调用 openssl 生成证书和私钥
openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
    -keyout ssl/server.key \
    -out ssl/server.crt \
    -config "$TEMP_CNF" > /dev/null 2>&1

# 清理临时文件
rm -f "$TEMP_CNF"

# 检查是否生成成功
if [ -f "ssl/server.crt" ] && [ -f "ssl/server.key" ]; then
    # 设置安全的文件权限
    chmod 644 ssl/server.crt
    chmod 600 ssl/server.key
    
    echo " 证书生成成功！"
    echo "   - 证书路径: ssl/server.crt"
    echo "   - 私钥路径: ssl/server.key"
    echo "   - 绑定域名/IP: ${SERVER_HOST}"
    echo ""
    echo "现在你可以运行 'docker compose up -d' 启动服务了！"
else
    echo " 证书生成失败，请检查 openssl 报错信息。"
    exit 1
fi
