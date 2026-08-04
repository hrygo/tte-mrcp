#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
LOG_FILE="$BUILD_DIR/server.log"
PYTHON_BIN="${PYTHON:-python3}"

mkdir -p "$BUILD_DIR"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
  echo "Error: python3 not found. Please install python3 or set PYTHON to a valid path." >&2
  exit 1
fi

# Check if unimrcpserver is already running
cd "$BUILD_DIR"

FOUND_PIDS=$(pgrep -f "platforms/unimrcp-server/unimrcpserver" 2>/dev/null || true)

# Filter out the script itself
SCRIPT_PID=$$
FOUND_PIDS=$(echo "$FOUND_PIDS" | grep -v "$SCRIPT_PID" 2>/dev/null || true)

if [ -n "$FOUND_PIDS" ]; then
    echo "错误：unimrcpserver 已经在运行 (PID: $FOUND_PIDS)" >&2
    echo "请先运行 ./shell/stop_server.sh 停止现有进程" >&2
    exit 1
fi

nohup "$PYTHON_BIN" -u - <<'PY' >/dev/null 2>&1 &
import os
import sys
import time
import subprocess
import threading

log_file = os.path.abspath("server.log")
max_bytes = 100 * 1024 * 1024

# Check if stdbuf is available
def has_stdbuf():
    try:
        subprocess.run(["stdbuf", "--version"], capture_output=True)
        return True
    except Exception:
        return False

cmd_base = [
    "./platforms/unimrcp-server/unimrcpserver",
    "-w",
    "-o",
    "1",
    "../conf/unimrcpserver.xml",
]

if not os.path.exists(cmd_base[0]):
    sys.stderr.write(f"Error: {cmd_base[0]} not found. Build first.\n")
    sys.exit(1)

# Use stdbuf to force line buffering if available
if has_stdbuf():
    cmd = ["stdbuf", "-oL", "-eL"] + cmd_base
else:
    # Fallback: try unbuffer or script command
    cmd = cmd_base

def date_str(ts=None):
    return time.strftime("%Y%m%d", time.localtime(ts if ts is not None else time.time()))

def rotate_log():
    if os.path.exists(log_file):
        base = f"{log_file}.{date_str()}"
        dst = base
        idx = 1
        while os.path.exists(dst):
            dst = f"{base}.{idx}"
            idx += 1
        os.rename(log_file, dst)

# Rotate at start if needed
if os.path.exists(log_file):
    mtime_date = date_str(os.path.getmtime(log_file))
    size = os.path.getsize(log_file)
    if mtime_date != date_str() or size >= max_bytes:
        rotate_log()

log_fp = open(log_file, "ab", buffering=0)
bytes_written = os.path.getsize(log_file) if os.path.exists(log_file) else 0
current_date = date_str()
write_lock = threading.Lock()

def write_line(line):
    """Write a line to log file with rotation check"""
    global bytes_written, current_date, log_fp

    if not line:
        return

    with write_lock:
        now_date = date_str()
        if now_date != current_date:
            log_fp.close()
            rotate_log()
            log_fp = open(log_file, "ab", buffering=0)
            bytes_written = 0
            current_date = now_date
        if bytes_written + len(line) > max_bytes:
            log_fp.close()
            rotate_log()
            log_fp = open(log_file, "ab", buffering=0)
            bytes_written = 0
        log_fp.write(line)
        log_fp.flush()
        bytes_written += len(line)

def reader_thread(stream):
    """Read lines from stream and write to log immediately"""
    try:
        for line in iter(stream.readline, b''):
            if line:
                write_line(line)
    except Exception as e:
        pass
    finally:
        stream.close()

# Start process with PTY if possible for truly unbuffered output
use_pty = False
master_fd = None

try:
    import pty
    master_fd, slave_fd = pty.openpty()
    use_pty = True
except Exception:
    use_pty = False

if use_pty:
    # PTY provides line-buffered output naturally
    proc = subprocess.Popen(
        cmd_base,
        stdin=subprocess.DEVNULL,
        stdout=slave_fd,
        stderr=slave_fd,
        close_fds=True,
    )
    os.close(slave_fd)

    try:
        while True:
            try:
                data = os.read(master_fd, 4096)
                if not data:
                    break
                write_line(data)
            except OSError:
                break

            if proc.poll() is not None:
                # Read remaining data
                while True:
                    try:
                        data = os.read(master_fd, 4096)
                        if not data:
                            break
                        write_line(data)
                    except OSError:
                        break
                break
    finally:
        os.close(master_fd)
else:
    # Fallback: use stdbuf + line-by-line reading
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=1,  # Line buffered
    )

    try:
        # Read line by line for real-time output
        for line in iter(proc.stdout.readline, b''):
            if line:
                write_line(line)
            if proc.poll() is not None:
                # Read remaining
                for remaining_line in proc.stdout:
                    write_line(remaining_line)
                break
    except Exception as e:
        pass
    finally:
        try:
            proc.stdout.close()
        except Exception:
            pass

try:
    log_fp.close()
except Exception:
    pass
try:
    proc.wait()
except Exception:
    pass
PY

echo "Started unimrcpserver in background. Log: $LOG_FILE"
echo "View logs: tail -f $LOG_FILE"
