#!/bin/bash
set -x  # 开启调试模式，打印每一步执行

# ===================== 配置项 =====================
TTS_URL="http://40.20.85.37:8091/v1/audio/speech"
TEXT="Hello, how are you?"
VOICE="vivian"
LANGUAGE="English"
RESPONSE_FORMAT="pcm"
SAMPLE_RATE=24000
TIMEOUT=20
# 音频保存配置：当前目录 + 时间戳命名
AUDIO_SAVE_PATH="./tts_audio_$(date +%Y%m%d_%H%M%S).pcm"
# ================================================

# 临时文件（脚本结束自动清理）
TEMP_AUDIO=$(mktemp /tmp/tts_audio.XXXXXX)
TEMP_ERR=$(mktemp /tmp/tts_curl_err.XXXXXX)
FIRST_BYTE_TIME_FILE=$(mktemp /tmp/tts_first_byte.XXXXXX)
echo "0" > "$FIRST_BYTE_TIME_FILE"

# 创建FIFO命名管道用于实时检测首字节
FIFO_PIPE=$(mktemp -u /tmp/tts_pipe.XXXXXX)
mkfifo "$FIFO_PIPE"

trap 'rm -f "$TEMP_AUDIO" "$TEMP_ERR" "$FIRST_BYTE_TIME_FILE" "$FIFO_PIPE"' EXIT

echo -e "\033[36m===== [调试] 脚本启动 =====\033[0m"
echo "[调试] 临时音频文件: $TEMP_AUDIO"
echo "[调试] 临时错误文件: $TEMP_ERR"
echo "[调试] FIFO管道: $FIFO_PIPE"
echo "[调试] 首字节时间文件: $FIRST_BYTE_TIME_FILE"
echo "[调试] 最终保存音频路径: $AUDIO_SAVE_PATH"
echo "[调试] 请求URL: $TTS_URL"
echo "[调试] 开始时间戳: $(date +%s%N)"

START_TIME=$(($(date +%s%N)/1000000))
FIRST_PACKET_LATENCY=""

echo -e "\033[36m===== [调试] 启动curl请求 =====\033[0m"

# 后台进程：从FIFO读取第一个字节并记录时间
(
  # 使用dd读取第一个字节（超时5秒）
  FIRST_BYTE=$({ timeout 5 dd bs=1 count=1 2>/dev/null || echo ""; } < "$FIFO_PIPE")

  if [ -n "$FIRST_BYTE" ]; then
    FIRST_BYTE_MS=$(($(date +%s%N)/1000000))
    echo "$FIRST_BYTE_MS" > "$FIRST_BYTE_TIME_FILE"
    echo -e "\033[32m[后台检测] 首字节到达！时间戳: $FIRST_BYTE_MS\033[0m" >&2
  else
    echo -e "\033[31m[后台检测] 超时：未收到首字节\033[0m" >&2
  fi

  # 继续消费FIFO中的剩余数据以防止阻塞
  cat > /dev/null < "$FIFO_PIPE"
) &

FIFO_READER_PID=$!
echo "[调试] FIFO读取进程PID: $FIFO_READER_PID"

# 1. 执行curl，同时保存音频流并实时推送到FIFO进行首字节检测
curl -sS -X POST "$TTS_URL" \
  -H "Content-Type: application/json" \
  -d '{
      "input": "'"$TEXT"'",
      "voice": "'"$VOICE"'",
      "language": "'"$LANGUAGE"'",
      "stream": true,
      "response_format": "'"$RESPONSE_FORMAT"'"
    }' --no-buffer 2>"$TEMP_ERR" | tee "$TEMP_AUDIO" > "$FIFO_PIPE"

CURL_EXIT_CODE=$?

# 关闭FIFO管道，让后台进程知道数据传输结束
exec 3>"$FIFO_PIPE"
exec 3<&-
wait $FIFO_READER_PID 2>/dev/null

echo -e "\033[36m===== [调试] curl执行完成 =====\033[0m"
echo "[调试] curl退出码: $CURL_EXIT_CODE"
echo "[调试] curl错误日志:"
cat "$TEMP_ERR"
echo "[调试] 临时音频文件大小: $(stat -c%s "$TEMP_AUDIO" 2>/dev/null || stat -f%z "$TEMP_AUDIO" 2>/dev/null) 字节"
echo "[调试] FIFO读取进程已结束"

# 2. 从后台进程获取首字节到达时间
FIRST_BYTE_MS=$(cat "$FIRST_BYTE_TIME_FILE")
echo "[调试] 读取到的首字节时间戳: $FIRST_BYTE_MS"

if [ "$FIRST_BYTE_MS" != "0" ] && [ -s "$TEMP_AUDIO" ]; then
  echo -e "\033[36m===== [调试] 统计首包耗时 =====\033[0m"
  # 计算真正的首包延迟：从请求开始到第一个字节到达的时间
  FIRST_PACKET_LATENCY=$((FIRST_BYTE_MS - START_TIME))
  echo "[调试] 请求开始时间: $START_TIME ms"
  echo "[调试] 首字节到达时间: $FIRST_BYTE_MS ms"
  echo -e "\033[32m✅ 首包音频已收到！实时首包延迟: ${FIRST_PACKET_LATENCY} 毫秒\033[0m"

  # 核心新增：将临时音频文件复制到当前目录保留
  cp "$TEMP_AUDIO" "$AUDIO_SAVE_PATH"
  echo -e "\033[32m✅ 音频文件已保存到: $AUDIO_SAVE_PATH\033[0m"
  echo "[调试] 保存的音频文件大小: $(stat -c%s "$AUDIO_SAVE_PATH" 2>/dev/null || stat -f%z "$AUDIO_SAVE_PATH" 2>/dev/null) 字节"
else
  echo -e "\033[31m❌ 错误：临时音频文件为空，curl未返回数据\033[0m"
  exit 1
fi

# 3. 尝试播放音频（服务器无声卡时会失败，但不影响结果统计）
echo -e "\033[36m===== [调试] 尝试播放音频 =====\033[0m"
if command -v play &> /dev/null; then
  cat "$TEMP_AUDIO" | play -q -t raw -r "$SAMPLE_RATE" -e signed -b 16 -c 1 - 2>/tmp/tts_play_err.log
  PLAY_EXIT_CODE=$?
  echo "[调试] play退出码: $PLAY_EXIT_CODE"
  if [ $PLAY_EXIT_CODE -ne 0 ]; then
    echo "[调试] play错误日志:"
    cat /tmp/tts_play_err.log
    echo -e "\033[33m⚠️  警告：播放失败（服务器可能无音频设备），但首包数据已收到且音频文件已保存\033[0m"
  fi
else
  echo -e "\033[33m⚠️  警告：未安装sox(play命令)，跳过播放\033[0m"
fi

# 4. 最终结果输出
echo -e "\n\033[36m===== 执行结果 =====\033[0m"
echo "首包音频返回耗时：${FIRST_PACKET_LATENCY} 毫秒"
echo "音频文件保存路径：$AUDIO_SAVE_PATH"
echo "音频文件大小：$(stat -c%s "$AUDIO_SAVE_PATH" 2>/dev/null || stat -f%z "$AUDIO_SAVE_PATH" 2>/dev/null) 字节"
echo -e "\033[32m脚本执行完成 ✅\033[0m"

set +x  # 关闭调试模式