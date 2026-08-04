#!/usr/bin/env bash
set -euo pipefail

# ============================================
# 环境检测：生产环境 vs 本地环境
# ============================================
if [ -d "${HOME}/apps/tts-mrcp" ]; then
    # 生产环境
    APP_HOME="${HOME}/apps/tts-mrcp"
else
    # 本地环境：根据脚本位置推导
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    APP_HOME="$(dirname "$SCRIPT_DIR")"
fi

# Verify project directory exists
if [ ! -d "$APP_HOME" ]; then
    echo "✗ Project directory not found: $APP_HOME"
    exit 1
fi

# Check if unimrcpserver is running (match by process name)
FOUND_PIDS=$(pgrep -x "unimrcpserver" 2>/dev/null || true)

if [ -n "$FOUND_PIDS" ]; then
    PID_COUNT=$(echo "$FOUND_PIDS" | wc -l | tr -d ' ')
    if [ "$PID_COUNT" -eq 1 ]; then
        echo "✓ unimrcpserver is running (PID: $FOUND_PIDS)"
    else
        echo "✓ unimrcpserver is running ($PID_COUNT instances, PIDs: $(echo "$FOUND_PIDS" | tr '\n' ' ' | sed 's/ $//'))"
    fi
    exit 0
else
    echo "✗ unimrcpserver is NOT running"
    exit 1
fi
