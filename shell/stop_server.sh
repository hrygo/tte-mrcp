#!/usr/bin/env bash
set -euo pipefail

# Stop unimrcp server started by shell/start_server.sh

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

# Check if build dir exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Build dir not found: $BUILD_DIR" >&2
    exit 1
fi

cd "$BUILD_DIR"

# Find processes containing the unimrcpserver path in command line
echo "Searching for unimrcpserver processes..."

FOUND_PIDS=$(pgrep -f "platforms/unimrcp-server/unimrcpserver" 2>/dev/null || true)

# Filter out the script itself
SCRIPT_PID=$$
FOUND_PIDS=$(echo "$FOUND_PIDS" | grep -v "$SCRIPT_PID" 2>/dev/null || true)

if [ -z "$FOUND_PIDS" ]; then
    echo "unimrcpserver not running."
    exit 0
fi

echo "Found unimrcpserver PIDs: $FOUND_PIDS"

# Send SIGTERM for graceful shutdown
kill -TERM $FOUND_PIDS 2>/dev/null || true

# Wait for graceful shutdown (up to 5 seconds)
echo "Waiting for graceful shutdown..."
for i in {1..5}; do
    sleep 1
    REMAINING=""
    for pid in $FOUND_PIDS; do
        if kill -0 "$pid" 2>/dev/null; then
            REMAINING="$REMAINING $pid"
        fi
    done
    if [ -z "$REMAINING" ]; then
        echo "Stopped unimrcpserver gracefully."
        exit 0
    fi
    echo -n "."
done
echo ""

# Force kill remaining processes
if [ -n "$REMAINING" ]; then
    echo "Forcing shutdown (SIGKILL) for remaining PIDs: $REMAINING"
    for pid in $REMAINING; do
        kill -9 "$pid" 2>/dev/null || true
    done
    sleep 1

    # Final verification
    REMAINING=""
    for pid in $FOUND_PIDS; do
        if kill -0 "$pid" 2>/dev/null; then
            REMAINING="$REMAINING $pid"
        fi
    done

    if [ -n "$REMAINING" ]; then
        echo "Failed to stop unimrcpserver. Remaining PIDs: $REMAINING" >&2
        echo "You may need to manually kill these processes:" >&2
        echo "  kill -9 $REMAINING" >&2
        exit 1
    fi
fi

echo "Stopped unimrcpserver (forcibly if needed)."
exit 0
