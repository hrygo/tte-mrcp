#!/usr/bin/env bash
set -euo pipefail

LOG_DIR="/home/logs"
RETENTION_DAYS=7
LOG_FILE="$LOG_DIR/clean_logs.log"

# Ensure log directory exists
mkdir -p "$LOG_DIR"

# Log function with timestamp
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

# Clean logs older than RETENTION_DAYS
log "Cleaning logs older than $RETENTION_DAYS days in $LOG_DIR..."

DELETED=0
if find "$LOG_DIR" -type f -name "*.log" -mtime +$RETENTION_DAYS -print -delete 2>/dev/null; then
    DELETED=$?
    log "Log cleanup completed"
else
    log "No old logs found or error occurred"
fi

# Also clean any rotated log files (*.log.*, *.log.YYYYMMDD, etc.)
find "$LOG_DIR" -type f \( -name "*.log.*" -o -name "*.log.[0-9]*" \) -mtime +$RETENTION_DAYS -delete 2>/dev/null && log "Rotated logs cleaned" || true
