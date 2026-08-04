#!/usr/bin/env bash
set -euo pipefail

# ============================================
# 环境检测：生产环境 vs 本地环境
# ============================================
if [ -d "${HOME}/apps/tts-mrcp" ]; then
    # 生产环境
    APP_HOME="${HOME}/apps/tts-mrcp"
    SHELL_DIR="${HOME}/shell"
elif [ -d "${HOME}/shell" ]; then
    # 生产环境但项目目录不同
    APP_HOME="${HOME}/apps/tts-mrcp"
    SHELL_DIR="${HOME}/shell"
else
    # 本地环境：根据脚本位置推导
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    APP_HOME="$(dirname "$SCRIPT_DIR")"
    SHELL_DIR="$APP_HOME/shell"
fi

LOG_DIR="/home/logs"
LOG_FILE="$LOG_DIR/check_once.log"

# Ensure log directory exists
mkdir -p "$LOG_DIR"

# Log function with timestamp
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

# Check if unimrcpserver is running (match full command to avoid false positives)
if ! pgrep -f "unimrcpserver.*dirlayout.xml.*unimrcpserver.xml" >/dev/null 2>&1; then
    log "unimrcpserver is NOT running, attempting to start..."

    if [ -x "$SHELL_DIR/start_server.sh" ]; then
        if "$SHELL_DIR/start_server.sh" 2>&1 | tee -a "$LOG_FILE"; then
            log "unimrcpserver started successfully"
        else
            log "ERROR: Failed to start unimrcpserver (exit code: $?)"
        fi
    else
        log "ERROR: Start script not found or not executable: $SHELL_DIR/start_server.sh"
    fi
fi
