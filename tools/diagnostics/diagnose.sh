#!/bin/bash

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
PLUGIN_DIR="${PLUGIN_DIR:-$ROOT_DIR/plugin}"

echo "=========================================="
echo "UniMRCP 环境诊断"
echo "=========================================="

# 1. 检查服务器状态
echo ""
echo "1. 检查服务器状态..."
if pgrep -f "unimrcpserver" > /dev/null; then
    echo "✓ 服务器正在运行"
    lsof -i :8060 | head -3
else
    echo "✗ 服务器未运行"
    echo "  启动命令: $ROOT_DIR/platforms/unimrcp-server/unimrcpserver -r $ROOT_DIR -l 4 -o 3"
fi

# 2. 检查TTS后端服务
echo ""
echo "2. 检查TTS后端服务..."
if lsof -i :8091 > /dev/null 2>&1; then
    echo "✓ TTS服务器在端口8091监听"
    lsof -i :8091 | head -2
else
    echo "✗ TTS服务器未在端口8091监听"
fi

# 3. 检查ASR后端服务
echo ""
echo "3. 检查ASR后端服务..."
CONFIG_FILE="$ROOT_DIR/conf/unimrcpserver.xml"
if [ -f "$CONFIG_FILE" ]; then
    if ASR_ENDPOINT=$(python3 - "$CONFIG_FILE" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
engines = [
    node for node in root.iter("engine")
    if node.get("id") == "ASR-WebSocket-1"
    and node.get("name") == "asr_websocket"
]
if len(engines) != 1:
    raise SystemExit("expected exactly one ASR-WebSocket-1/asr_websocket engine")
params = {node.get("name"): node.get("value") for node in engines[0]}
values = [params.get(name, "") for name in ("funasr-host", "funasr-port", "funasr-path")]
if not all(values):
    raise SystemExit("missing funasr-host/funasr-port/funasr-path")
print("\n".join(values))
PY
    ); then
        ASR_HOST=$(printf '%s\n' "$ASR_ENDPOINT" | sed -n '1p')
        ASR_PORT=$(printf '%s\n' "$ASR_ENDPOINT" | sed -n '2p')
        ASR_PATH=$(printf '%s\n' "$ASR_ENDPOINT" | sed -n '3p')
    else
        ASR_HOST=""
        ASR_PORT=""
        ASR_PATH=""
    fi
    if [ -n "$ASR_HOST" ] && [ -n "$ASR_PORT" ] && [ -n "$ASR_PATH" ]; then
        echo "✓ ASR WebSocket endpoint configured: ws://$ASR_HOST:$ASR_PORT$ASR_PATH"
        if lsof -i :"$ASR_PORT" > /dev/null 2>&1; then
            echo "✓ ASR服务器在端口$ASR_PORT监听"
            lsof -i :"$ASR_PORT" | head -2
        else
            echo "✗ ASR服务器未在端口$ASR_PORT监听"
        fi
    else
        echo "✗ ASR WebSocket endpoint 缺少 funasr-host/funasr-port/funasr-path: $CONFIG_FILE"
    fi
else
    echo "✗ 配置文件不存在: $CONFIG_FILE"
fi

# 4. 检查插件
echo ""
echo "4. 检查插件..."
if [ -f "$PLUGIN_DIR/tts_websocket.so" ]; then
    echo "✓ TTS插件存在: $PLUGIN_DIR/tts_websocket.so"
else
    echo "✗ TTS插件不存在: $PLUGIN_DIR/tts_websocket.so"
fi

if [ -f "$PLUGIN_DIR/asr_websocket.so" ]; then
    echo "✓ ASR WebSocket 插件存在: $PLUGIN_DIR/asr_websocket.so"
else
    echo "✗ ASR WebSocket 插件不存在: $PLUGIN_DIR/asr_websocket.so"
fi

# 5. 测试SIP连接
echo ""
echo "5. 测试SIP连接..."
cat > /tmp/test_sip.exp << 'EOF'
#!/usr/bin/expect -f
set timeout 10
spawn ./platforms/umc/umc -r . -l 4 -o 1
expect ">*"
send "run synth uni2\r"
expect {
    "200 OK" { puts "SIP连接成功"; exp_continue }
    timeout { puts "SIP连接超时" }
}
expect ">*"
send "exit\r"
expect eof
EOF
chmod +x /tmp/test_sip.exp
(cd "$ROOT_DIR" && /tmp/test_sip.exp) 2>&1 | grep -E "SIP|连接|ready|active"

# 6. 检查配置文件
echo ""
echo "6. 检查配置文件..."
grep -A 2 "tts-host\|tts-port\|funasr-host\|funasr-port\|funasr-path" "$CONFIG_FILE" | head -15

echo ""
echo "=========================================="
echo "诊断完成"
echo "=========================================="
