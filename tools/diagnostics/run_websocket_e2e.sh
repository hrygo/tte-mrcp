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
mkdir -p "$WORK_DIR"

server_pid=""
tts_pid=""
asr_pid=""
config_backup="$WORK_DIR/unimrcpserver.xml.original"

cleanup() {
  set +e
  for pid in "$tts_pid" "$asr_pid" "$server_pid"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
  done
  for pid in "$tts_pid" "$asr_pid" "$server_pid"; do
    [ -n "$pid" ] && wait "$pid" 2>/dev/null || true
  done
  if [ -f "$config_backup" ]; then
    cp "$config_backup" "$PACKAGE_DIR/conf/unimrcpserver.xml"
  fi
}
trap cleanup EXIT

fail() {
  echo "websocket E2E failed: $*" >&2
  tail -n 100 "$WORK_DIR/server.log" 2>/dev/null || true
  exit 1
}

command -v expect >/dev/null || fail "expect is required"
[ -x "$PACKAGE_DIR/bin/unimrcpserver" ] || fail "missing $PACKAGE_DIR/bin/unimrcpserver"
[ -x "$PACKAGE_DIR/bin/umc" ] || fail "missing $PACKAGE_DIR/bin/umc"
[ -f "$PACKAGE_DIR/plugin/tts_websocket.so" ] || fail "missing packaged tts_websocket.so"
[ -f "$PACKAGE_DIR/plugin/asr_websocket.so" ] || fail "missing packaged asr_websocket.so"

python3 "$FIXTURE" --self-test
cp "$PACKAGE_DIR/conf/unimrcpserver.xml" "$config_backup"
python3 "$FIXTURE" --emit-config \
  "$config_backup" "$WORK_DIR/unimrcpserver.xml" \
  --tts-port "$TTS_PORT" --asr-port "$ASR_PORT"
cp "$WORK_DIR/unimrcpserver.xml" "$PACKAGE_DIR/conf/unimrcpserver.xml"

export LD_LIBRARY_PATH="$PACKAGE_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export UMC_BIN="$PACKAGE_DIR/bin/umc"
python3 "$FIXTURE" --service tts --host 127.0.0.1 --port "$TTS_PORT" \
  --mode "$MODE" --once 1 --report "$WORK_DIR/tts.jsonl" \
  >"$WORK_DIR/tts.fixture.log" 2>&1 &
tts_pid=$!
python3 "$FIXTURE" --service asr --host 127.0.0.1 --port "$ASR_PORT" \
  --mode "$MODE" --once 1 --report "$WORK_DIR/asr.jsonl" \
  >"$WORK_DIR/asr.fixture.log" 2>&1 &
asr_pid=$!

for fixture_log in "$WORK_DIR/tts.fixture.log" "$WORK_DIR/asr.fixture.log"; do
  for _ in $(seq 1 50); do
    grep -q '"event": "listening"' "$fixture_log" && break
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
grep -q "Failed to Load UniMRCP Server Document\|Failed to compose plugin path" "$WORK_DIR/server.log" && fail "plugin/config load failed"

run_umc() {
  local kind="$1"
  local output="$WORK_DIR/$kind.umc.log"
  timeout 50 expect "$UMC_EXPECT" "$kind" uni2 "$PACKAGE_DIR" 0 >"$output" 2>&1 || {
    cat "$output" >&2
    fail "$kind UMC invocation failed"
  }
  grep -q "RESULT:SUCCESS" "$output" || { cat "$output" >&2; fail "$kind did not complete successfully"; }
  if [ "$kind" = synth ]; then
    grep -Eq 'r:[1-9][0-9]* l:[0-9]+ j:[0-9]+' "$output" || fail "TTS produced no RTP media"
  else
    grep -q "NLSML:.*fixture-asr" "$output" || fail "ASR NLSML does not contain fixture text"
  fi
}

run_umc synth
run_umc recog

python3 - "$WORK_DIR/tts.jsonl" "$WORK_DIR/asr.jsonl" <<'PY'
import json
import sys

for path, service in zip(sys.argv[1:], ("tts", "asr")):
    records = [json.loads(line) for line in open(path, encoding="utf-8") if line.strip()]
    if len(records) != 1:
        raise SystemExit(f"{service}: expected one mock session, got {len(records)}")
    record = records[0]
    if record["outcome"] != "final-sent":
        raise SystemExit(f"{service}: outcome={record['outcome']}")
    if service == "tts":
        types = [item.get("type") for item in record["text_messages"]]
        for required in ("session.config", "input.text", "input.done"):
            if required not in types:
                raise SystemExit(f"tts: missing {required} in {types}")
    elif record["audio_bytes"] <= 0:
        raise SystemExit("asr: mock received no audio")
print("websocket E2E mock assertions passed")
PY

echo "websocket E2E passed: TTS RTP + ASR NLSML against loopback mocks"
