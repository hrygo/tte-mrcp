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
if lsof -i :8022 > /dev/null 2>&1; then
    echo "✓ TTS服务器在端口8022监听"
    lsof -i :8022 | head -2
else
    echo "✗ TTS服务器未在端口8022监听"
fi

# 3. 检查ASR后端服务
echo ""
echo "3. 检查ASR后端服务..."
echo "  ASR服务器地址: 40.20.85.37:8888 (根据代码)"

# 4. 检查插件
echo ""
echo "4. 检查插件..."
if [ -f "$PLUGIN_DIR/tts_websocket.so" ]; then
    echo "✓ TTS插件存在: $PLUGIN_DIR/tts_websocket.so"
else
    echo "✗ TTS插件不存在: $PLUGIN_DIR/tts_websocket.so"
fi

if [ -f "$PLUGIN_DIR/demorecog.so" ]; then
    echo "✓ ASR插件存在: $PLUGIN_DIR/demorecog.so"
else
    echo "✗ ASR插件不存在: $PLUGIN_DIR/demorecog.so"
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
grep -A 2 "tts-host\|tts-port" "$ROOT_DIR/conf/unimrcpserver.xml" | head -10

echo ""
echo "=========================================="
echo "诊断完成"
echo "=========================================="
