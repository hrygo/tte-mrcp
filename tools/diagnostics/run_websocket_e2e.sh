#!/usr/bin/env bash
set -euo pipefail

# Runs both production WebSocket plugins against loopback doubles.  The
# packaged root is passed explicitly so this gate proves plugin loading and
# runtime linkage from the distributable tree.
ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
PACKAGE_DIR="${PACKAGE_DIR:-$ROOT_DIR}"
FIXTURE="$ROOT_DIR/tools/diagnostics/websocket_e2e_fixture.py"
UMC_EXPECT="$ROOT_DIR/tests/integration/umc_test.exp"
WORK_DIR="${E2E_WORK_DIR:-$ROOT_DIR/websocket-e2e-results}"
TTS_PORT="${TTS_PORT:-18091}"
ASR_PORT="${ASR_PORT:-18022}"
MODE="${E2E_MODE:-split}"
FIXTURE_TIMEOUT="${E2E_FIXTURE_TIMEOUT:-20}"
mkdir -p "$WORK_DIR"
rm -f "$WORK_DIR"/tts.jsonl "$WORK_DIR"/asr.jsonl \
  "$WORK_DIR"/tts.fixture.log "$WORK_DIR"/asr.fixture.log "$WORK_DIR"/server.log

server_pid=""
tts_pid=""
asr_pid=""
config_backup="$WORK_DIR/unimrcpserver.xml.original"

cleanup() {
  set +e
  # SIGTERM first, then wait up to 5 seconds; escalate to SIGKILL if needed.
  for pid in "$tts_pid" "$asr_pid" "$server_pid"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
  done
  for wait_i in 1 2 3 4 5; do
    local alive=0
    for pid in "$tts_pid" "$asr_pid" "$server_pid"; do
      [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && alive=1
    done
    [ "$alive" = 0 ] && break
    sleep 1
  done
  for pid in "$tts_pid" "$asr_pid" "$server_pid"; do
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
  done
  for pid in "$tts_pid" "$asr_pid" "$server_pid"; do
    [ -n "$pid" ] && wait "$pid" 2>/dev/null || true
  done
  if [ -f "$config_backup" ]; then
    cp "$config_backup" "$PACKAGE_DIR/conf/unimrcpserver.xml" ||
      echo "websocket E2E cleanup: failed to restore server config" >&2
  fi
}
trap cleanup EXIT

fail() {
  echo "websocket E2E failed: $*" >&2
  tail -n 100 "$WORK_DIR/server.log" 2>/dev/null || true
  exit 1
}

command -v expect >/dev/null || fail "expect is required (install expect before running this gate)"
[ -x "$PACKAGE_DIR/bin/unimrcpserver" ] || fail "missing $PACKAGE_DIR/bin/unimrcpserver"
[ -x "$PACKAGE_DIR/bin/umc" ] || fail "missing $PACKAGE_DIR/bin/umc"
[ -f "$PACKAGE_DIR/plugin/tts_websocket.so" ] || fail "missing packaged tts_websocket.so"
[ -f "$PACKAGE_DIR/plugin/asr_websocket.so" ] || fail "missing packaged asr_websocket.so"
[ -f "$PACKAGE_DIR/conf/unimrcpserver.xml" ] || fail "missing packaged server config"

python3 "$FIXTURE" --self-test
cp "$PACKAGE_DIR/conf/unimrcpserver.xml" "$config_backup"
python3 "$FIXTURE" --emit-config \
  "$config_backup" "$WORK_DIR/unimrcpserver.xml" \
  --tts-port "$TTS_PORT" --asr-port "$ASR_PORT"
cp "$WORK_DIR/unimrcpserver.xml" "$PACKAGE_DIR/conf/unimrcpserver.xml"

export LD_LIBRARY_PATH="$PACKAGE_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export UMC_BIN="$PACKAGE_DIR/bin/umc"
python3 "$FIXTURE" --service tts --host 127.0.0.1 --port "$TTS_PORT" \
  --mode "$MODE" --timeout "$FIXTURE_TIMEOUT" --once 1 --report "$WORK_DIR/tts.jsonl" \
  >"$WORK_DIR/tts.fixture.log" 2>&1 &
tts_pid=$!
python3 "$FIXTURE" --service asr --host 127.0.0.1 --port "$ASR_PORT" \
  --mode "$MODE" --timeout "$FIXTURE_TIMEOUT" --once 1 --report "$WORK_DIR/asr.jsonl" \
  >"$WORK_DIR/asr.fixture.log" 2>&1 &
asr_pid=$!

for fixture_log in "$WORK_DIR/tts.fixture.log" "$WORK_DIR/asr.fixture.log"; do
  for _ in $(seq 1 50); do
    grep -q '"event": "listening"' "$fixture_log" && {
      fixture_pid_var=tts_pid
      [[ "$fixture_log" = *asr* ]] && fixture_pid_var=asr_pid
      fixture_pid="${!fixture_pid_var}"
      kill -0 "$fixture_pid" 2>/dev/null || fail "fixture exited after listening: $fixture_log"
      break
    }
    sleep 0.1
  done
  grep -q '"event": "listening"' "$fixture_log" || fail "fixture did not become ready: $fixture_log"
done

cd "$PACKAGE_DIR"
env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" stdbuf -oL -eL \
  "$PACKAGE_DIR/bin/unimrcpserver" -r "$PACKAGE_DIR" -l 6 -o 1 -w \
  < /dev/null >"$WORK_DIR/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 60); do
  grep -q "Create MRCPv2 Profile" "$WORK_DIR/server.log" && break
  kill -0 "$server_pid" 2>/dev/null || fail "unimrcpserver exited early"
  sleep 1
done
grep -q "Create MRCPv2 Profile" "$WORK_DIR/server.log" || fail "server did not become ready"
kill -0 "$server_pid" 2>/dev/null || fail "unimrcpserver exited after becoming ready"
grep -q "Failed to Load UniMRCP Server Document\|Failed to compose plugin path" "$WORK_DIR/server.log" && fail "plugin/config load failed"
# A server that cannot bind its SIP/MRCP/RTSP sockets still logs
# "MRCP Server Started", so the profile-ready check above is not
# sufficient.  Detect the bind failures explicitly so a leftover
# server from a previous run cannot silently poison the E2E gate.
if grep -qE "Failed to Create Listening Socket|Failed to Run Sofia-SIP Task|Failed to Create NUA" \
   "$WORK_DIR/server.log"; then
  fail "server failed to bind signaling ports (stale server from a previous run?)"
fi

run_umc() {
  local kind="$1"
  local output="$WORK_DIR/$kind.umc.log"
  set +e
  timeout 50 expect "$UMC_EXPECT" "$kind" uni2 "$PACKAGE_DIR" 0 >"$output" 2>&1
  local status=$?
  set -e
  if [ "$MODE" = close ]; then
    grep -q "RESULT:SUCCESS" "$output" && fail "$kind unexpectedly succeeded after fixture close"
    return 0
  fi
  case "$status" in
    0) ;;
    124) cat "$output" >&2; fail "$kind UMC timed out (exit 124)" ;;
    127) cat "$output" >&2; fail "$kind UMC command or dependency was not found (exit 127)" ;;
    *) cat "$output" >&2; fail "$kind UMC/Expect failed (exit $status)" ;;
  esac
  grep -q "RESULT:SUCCESS" "$output" || { cat "$output" >&2; fail "$kind did not complete successfully"; }
  if [ "$kind" = synth ]; then
    grep -Eq 'r:[1-9][0-9]* l:[0-9]+ j:[0-9]+' "$output" || fail "TTS produced no RTP media"
  else
    grep -q "NLSML:.*fixture-asr" "$output" || fail "ASR NLSML does not contain fixture text"
  fi
}

run_umc synth
run_umc recog

python3 - "$MODE" "$WORK_DIR/tts.jsonl" "$WORK_DIR/asr.jsonl" <<'PY'
import json
import sys

mode = sys.argv[1]
for path, service in zip(sys.argv[2:], ("tts", "asr")):
    try:
        with open(path, encoding="utf-8") as stream:
            records = [json.loads(line) for line in stream if line.strip()]
    except FileNotFoundError as error:
        raise SystemExit(f"{service}: report missing: {error.filename}")
    except OSError as error:
        raise SystemExit(f"{service}: report unreadable: {error}")
    if not records:
        raise SystemExit(f"{service}: report exists but is empty")
    if len(records) != 1:
        raise SystemExit(f"{service}: expected one mock session, got {len(records)}")
    record = records[0]
    expected_outcome = "injected-close" if mode == "close" else "final-sent"
    if record["outcome"] != expected_outcome:
        raise SystemExit(f"{service}: outcome={record['outcome']}")
    if mode == "close":
        continue
    if service == "tts":
        types = [item.get("type") for item in record["text_messages"]]
        for required in ("session.config", "input.text", "input.done"):
            if required not in types:
                raise SystemExit(f"tts: missing {required} in {types}")
        if record["audio_sample_rate"] != 24000:
            raise SystemExit(f"tts: unexpected sample rate {record['audio_sample_rate']}")
        # The mock fixture generates 240ms of audio (11520 bytes at 24kHz).
        # The threshold ensures the fixture sent a meaningful amount of audio
        # without requiring a specific duration.  5000 bytes ≈ 100ms minimum.
        if record["audio_frames"] < 2 or record["audio_bytes"] <= 5000:
            raise SystemExit(
                f"tts: insufficient audio evidence frames={record['audio_frames']} bytes={record['audio_bytes']}"
            )
    elif record["audio_bytes"] <= 0:
        raise SystemExit("asr: mock received no audio")
print("websocket E2E mock assertions passed")
PY

echo "websocket E2E passed: TTS RTP + ASR NLSML against loopback mocks"
