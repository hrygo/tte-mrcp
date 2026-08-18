#!/usr/bin/env bash

set -euo pipefail
test -f "$GITHUB_WORKSPACE/$ZIP_NAME"
docker run --interactive --rm \
  --volume "$GITHUB_WORKSPACE:/work" \
  --workdir /work \
  --env GITHUB_WORKSPACE=/work \
  --env TARGET \
  --env ZIP_NAME \
  --env CONCURRENCY \
  --env FAULT_CONNECTION \
  "$VERIFY_IMAGE" \
  bash -seu <<'CONTAINER_SCRIPT'
set -o pipefail

dnf -y install --setopt=install_weak_deps=False \
  expect python3 file findutils binutils procps-ng unzip nss-softokn-freebl
# nss-softokn-freebl: the rhel7 package bundles CentOS 7 libcrypt
# (DT_NEEDED libfreebl3.so), which must resolve on the verify image
# just like it does on the target RHEL 7 / Kylin systems.
# The fixture requires Python >= 3.7 (PEP 563 future annotations);
# Rocky Linux 8's default python3 is 3.6, so enable the python39 module.
dnf -y module enable python39
dnf -y install python39
ln -sfn /usr/bin/python3.9 /usr/local/bin/python3
python3 --version

cd "$GITHUB_WORKSPACE"
rm -rf verify-root
mkdir -p verify-root
cd verify-root
unzip -q "$GITHUB_WORKSPACE/$ZIP_NAME"
package_dir="$GITHUB_WORKSPACE/verify-root/opt/tte-mrcp"
# LD_LIBRARY_PATH is intentionally NOT exported globally: the
# bundled RHEL 7 libs (e.g. libcrypt -> libfreebl3) must not shadow
# system libraries for python3/fixture. It is applied per-command
# to the packaged binaries only.
PACKAGE_LIB="$package_dir/lib"
export PATH="$package_dir/bin:$PATH"
mkdir -p "$package_dir/log" "$package_dir/var"

# 1. Fixture self-test (stdlib only, no third-party dependencies)
python3 "$GITHUB_WORKSPACE/tools/diagnostics/funasr_ws_fixture.py" --self-test

# 2. Runtime dependency inspection: every ELF must resolve
for candidate in "$package_dir"/bin/* "$package_dir"/plugin/*.so; do
  [ -f "$candidate" ] || continue
  if file "$candidate" | grep -q ELF; then
    if LD_LIBRARY_PATH="$PACKAGE_LIB" ldd "$candidate" 2>&1 | grep -q "not found"; then
      echo "Unresolved dependency for $candidate" >&2
      LD_LIBRARY_PATH="$PACKAGE_LIB" ldd "$candidate" >&2 || true
      exit 1
    fi
  fi
done

# 3. Emit loopback server config: exactly one canonical ASR engine -> fixture
python3 "$GITHUB_WORKSPACE/tools/diagnostics/funasr_ws_fixture.py" \
  --emit-config "$GITHUB_WORKSPACE/conf/unimrcpserver.xml" \
  "$GITHUB_WORKSPACE/verify-unimrcpserver.xml" \
  --host 127.0.0.1 --port 8022 --path /ws/asr
cp "$GITHUB_WORKSPACE/verify-unimrcpserver.xml" \
   "$package_dir/conf/unimrcpserver.xml"
python3 - "$package_dir/conf/unimrcpserver.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
engines = [
    node for node in root.iter("engine")
    if node.get("id") == "ASR-WebSocket-1"
]
assert len(engines) == 1
assert engines[0].get("name") == "asr_websocket"
params = {node.get("name"): node.get("value") for node in engines[0]}
assert params["funasr-host"] == "127.0.0.1"
assert params["funasr-port"] == "8022"
assert params["funasr-path"] == "/ws/asr"
mappings = [
    node for node in root.iter("resource")
    if node.get("id") == "speechrecog"
    and node.get("engine") is not None
]
assert len(mappings) == 1
assert mappings[0].get("engine") == "ASR-WebSocket-1"
PY

# 4. Start the loopback fixture with fault injection
python3 "$GITHUB_WORKSPACE/tools/diagnostics/funasr_ws_fixture.py" \
  --host 127.0.0.1 --port 8022 --path /ws/asr \
  --mode split-payload \
  --fault-connection "$FAULT_CONNECTION" \
  --report "$GITHUB_WORKSPACE/fixture.jsonl" \
  > "$GITHUB_WORKSPACE/fixture.log" 2>&1 &
fixture_pid=$!
trap 'kill "$fixture_pid" "$server_pid" 2>/dev/null || true' EXIT

for i in $(seq 1 30); do
  if grep -q '"event": "listening"' "$GITHUB_WORKSPACE/fixture.log"; then
    break
  fi
  if ! kill -0 "$fixture_pid" 2>/dev/null; then
    echo "fixture exited early" >&2
    cat "$GITHUB_WORKSPACE/fixture.log" >&2 || true
    exit 1
  fi
  sleep 1
done
grep -q '"event": "listening"' "$GITHUB_WORKSPACE/fixture.log"

# 5. Start unimrcpserver from the packaged tree (console log only)
cd "$package_dir"
stdbuf -oL -eL env LD_LIBRARY_PATH="$PACKAGE_LIB" \
  bin/unimrcpserver \
  -r "$package_dir" -l 6 -o 1 -w \
  < /dev/null \
  > "$GITHUB_WORKSPACE/server.log" 2>&1 &
server_pid=$!

ready=0
for i in $(seq 1 60); do
  if grep -q "Create MRCPv2 Profile" "$GITHUB_WORKSPACE/server.log"; then
    ready=1
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "unimrcpserver exited early" >&2
    tail -n 120 "$GITHUB_WORKSPACE/server.log" >&2 || true
    exit 1
  fi
  sleep 1
done
if [ "$ready" != 1 ]; then
  echo "unimrcpserver did not become ready" >&2
  tail -n 120 "$GITHUB_WORKSPACE/server.log" >&2 || true
  exit 1
fi
sleep 2

# 6. Assert config/plugin load did not fail
if grep -qE "Failed to Load UniMRCP Server Document|Failed to compose plugin path" \
   "$GITHUB_WORKSPACE/server.log"; then
  echo "server config/plugin load failure" >&2
  exit 1
fi

# 7. UMC wrapper expected by the stress script: injects the packaged
# library path only for umc, leaving python3/expect untouched.
mkdir -p "$GITHUB_WORKSPACE/platforms/umc"
printf '%s\n' \
  '#!/bin/bash' \
  "export LD_LIBRARY_PATH=\"$PACKAGE_LIB\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\"" \
  "exec \"$PACKAGE_LIB/../bin/umc\" \"\$@\"" \
  > "$GITHUB_WORKSPACE/platforms/umc/umc"
chmod +x "$GITHUB_WORKSPACE/platforms/umc/umc"

# 8. 20-concurrent split-payload loopback experiment (1 warmup)
cd "$GITHUB_WORKSPACE"
bash tools/stress/stress_test_improved.sh \
  -t asr -c "$CONCURRENCY" -i 1 \
  -r "$GITHUB_WORKSPACE" \
  -a "$GITHUB_WORKSPACE/data" \
  --server-log="$GITHUB_WORKSPACE/server.log" \
  --fixture-report="$GITHUB_WORKSPACE/fixture.jsonl" \
  --pacing-json="$GITHUB_WORKSPACE/pacing.json" \
  --warmup=1 \
  --fault-connection="$FAULT_CONNECTION"

# 9. Assert ASR WebSocket pacing acceptance thresholds
python3 - "$GITHUB_WORKSPACE/pacing.json" "$CONCURRENCY" <<'PY'
import json
import sys

pacing_path, expected_requests = sys.argv[1], int(sys.argv[2])
with open(pacing_path, encoding="utf-8") as handle:
    report = json.load(handle)
problems = []
if report.get("requests") != expected_requests:
    problems.append(f"requests={report.get('requests')} != {expected_requests}")
if report.get("overrun_events", -1) != 0:
    problems.append(f"overrun_events={report.get('overrun_events')}")
if report.get("abnormal_closes", -1) != 0:
    problems.append(f"abnormal_closes={report.get('abnormal_closes')}")
if report.get("successes", -1) < expected_requests - 1:
    problems.append(f"successes={report.get('successes')} < {expected_requests - 1}")
p99 = report.get("nonfault_frame_gap_p99_ms")
maximum = report.get("nonfault_frame_gap_max_ms")
if p99 is not None and p99 >= 100:
    problems.append(f"nonfault_frame_gap_p99_ms={p99} >= 100")
if maximum is not None and maximum >= 250:
    problems.append(f"nonfault_frame_gap_max_ms={maximum} >= 250")
if problems:
    print(json.dumps(report, ensure_ascii=False, indent=2))
    raise SystemExit("\n".join(problems))
print(
    "pacing gate OK: "
    f"requests={report.get('requests')} successes={report.get('successes')} "
    f"p99={p99}ms max={maximum}ms overrun={report.get('overrun_events')}"
)
PY

# The stress-test server and fixture have served their purpose.
# The E2E gate below starts its own unimrcpserver and per-service
# fixtures on the same SIP/MRCP/RTSP ports, so the stress
# processes must be torn down first.  Without this the second
# server logs "Failed to Create Listening Socket" / "Failed to
# Run Sofia-SIP Task" and the UMC driver receives RESULT:NODATA.
#
# Tear-down protocol: SIGTERM first, then wait up to 5 seconds;
# if either process is still alive, escalate to SIGKILL.  This
# avoids hanging the job if a process ignores SIGTERM (unimrcpserver
# in particular has been observed to linger in shutdown).
kill "$server_pid" "$fixture_pid" 2>/dev/null || true
for wait_i in 1 2 3 4 5; do
  kill -0 "$server_pid" 2>/dev/null || kill -0 "$fixture_pid" 2>/dev/null || break
  sleep 1
done
kill -0 "$server_pid" 2>/dev/null && kill -9 "$server_pid" 2>/dev/null || true
kill -0 "$fixture_pid" 2>/dev/null && kill -9 "$fixture_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
wait "$fixture_pid" 2>/dev/null || true
# Reclaim the stress log so the E2E runner gets a clean server.log
# path; the file is recreated inside the loop per mode.
rm -f "$GITHUB_WORKSPACE/server.log"

# 10. TTS 20-concurrent stress test
#     websocket_e2e_fixture.py --emit-config patches both TTS and ASR engine
#     endpoints in the server XML.  Only the TTS mock is started because
#     TTS sessions never trigger the ASR engine.
python3 "$GITHUB_WORKSPACE/tools/diagnostics/websocket_e2e_fixture.py" \
  --emit-config "$GITHUB_WORKSPACE/conf/unimrcpserver.xml" \
    "$GITHUB_WORKSPACE/tts-unimrcpserver.xml" \
  --host 127.0.0.1 --tts-port 18091 --asr-port 18022
cp "$GITHUB_WORKSPACE/tts-unimrcpserver.xml" "$package_dir/conf/unimrcpserver.xml"

python3 "$GITHUB_WORKSPACE/tools/diagnostics/websocket_e2e_fixture.py" \
  --service tts --host 127.0.0.1 --port 18091 --mode normal \
  --report "$GITHUB_WORKSPACE/tts_fixture.jsonl" \
  > "$GITHUB_WORKSPACE/tts_fixture.log" 2>&1 &
tts_fixture_pid=$!

for i in $(seq 1 15); do
  if grep -q '"event": "listening"' "$GITHUB_WORKSPACE/tts_fixture.log" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$tts_fixture_pid" 2>/dev/null; then
    echo "TTS fixture exited early" >&2
    cat "$GITHUB_WORKSPACE/tts_fixture.log" >&2 || true
    exit 1
  fi
  sleep 1
done
grep -q '"event": "listening"' "$GITHUB_WORKSPACE/tts_fixture.log"

cd "$package_dir"
stdbuf -oL -eL env LD_LIBRARY_PATH="$PACKAGE_LIB" \
  bin/unimrcpserver -r "$package_dir" -l 6 -o 1 -w \
  < /dev/null > "$GITHUB_WORKSPACE/tts_server.log" 2>&1 &
tts_server_pid=$!

tts_ready=0
for i in $(seq 1 60); do
  if grep -q "Create MRCPv2 Profile" "$GITHUB_WORKSPACE/tts_server.log" 2>/dev/null; then
    tts_ready=1; break
  fi
  if ! kill -0 "$tts_server_pid" 2>/dev/null; then break; fi
  sleep 1
done
if [ "$tts_ready" != 1 ]; then
  echo "tts_server did not become ready" >&2
  tail -n 120 "$GITHUB_WORKSPACE/tts_server.log" >&2 || true
  exit 1
fi
sleep 2

cd "$GITHUB_WORKSPACE"
bash tools/stress/stress_test_improved.sh \
  -t tts -c "$CONCURRENCY" -i 1 \
  -r "$GITHUB_WORKSPACE" \
  --tts-save --tts-var-dir "$GITHUB_WORKSPACE/var"

# Assert TTS fixture outcomes: every session must complete with audio
python3 - "$GITHUB_WORKSPACE/tts_fixture.jsonl" "$CONCURRENCY" << 'PY'
import json, sys
fixture_path, expected = sys.argv[1], int(sys.argv[2])
sessions = []
with open(fixture_path) as f:
    for line in f:
        line = line.strip()
        if line:
            sessions.append(json.loads(line))
tts = [s for s in sessions if s.get("service") == "tts"]
problems = []
if len(tts) < expected:
    problems.append(f"tts_sessions={len(tts)} < {expected}")
bad = [s for s in tts if s.get("outcome") != "final-sent"]
if bad:
    outcomes = [s.get("outcome") for s in bad]
    problems.append(f"{len(bad)} sessions had outcome != final-sent: {outcomes}")
no_audio = [s for s in tts if s.get("audio_bytes", 0) == 0]
if no_audio:
    problems.append(f"{len(no_audio)} sessions sent 0 audio bytes")
if problems:
    print(json.dumps(tts, ensure_ascii=False, indent=2))
    raise SystemExit("\n".join(problems))
total_bytes = sum(s.get("audio_bytes", 0) for s in tts)
print(
    "TTS stress gate OK: "
    f"sessions={len(tts)} total_audio_bytes={total_bytes}"
)
PY

kill "$tts_server_pid" "$tts_fixture_pid" 2>/dev/null || true
for wait_i in 1 2 3 4 5; do
  kill -0 "$tts_server_pid" 2>/dev/null \
    || kill -0 "$tts_fixture_pid" 2>/dev/null \
    || break
  sleep 1
done
kill -0 "$tts_server_pid" 2>/dev/null && kill -9 "$tts_server_pid" 2>/dev/null || true
kill -0 "$tts_fixture_pid" 2>/dev/null && kill -9 "$tts_fixture_pid" 2>/dev/null || true
wait "$tts_server_pid" 2>/dev/null || true
wait "$tts_fixture_pid" 2>/dev/null || true
rm -f "$GITHUB_WORKSPACE/tts_server.log"

# 11. Mixed TTS+ASR 20-concurrent stress test (10 TTS + 10 ASR simultaneously)
#     stress_test_improved.sh -t mixed: even worker IDs -> TTS, odd -> ASR.
#     Port allocation is by worker_id so no SIP/RTP conflicts occur.
python3 "$GITHUB_WORKSPACE/tools/diagnostics/websocket_e2e_fixture.py" \
  --emit-config "$GITHUB_WORKSPACE/conf/unimrcpserver.xml" \
    "$GITHUB_WORKSPACE/mixed-unimrcpserver.xml" \
  --host 127.0.0.1 --tts-port 18091 --asr-port 18022
cp "$GITHUB_WORKSPACE/mixed-unimrcpserver.xml" "$package_dir/conf/unimrcpserver.xml"

python3 "$GITHUB_WORKSPACE/tools/diagnostics/websocket_e2e_fixture.py" \
  --service tts --host 127.0.0.1 --port 18091 --mode normal \
  > "$GITHUB_WORKSPACE/mixed_tts_fixture.log" 2>&1 &
mixed_tts_pid=$!

python3 "$GITHUB_WORKSPACE/tools/diagnostics/websocket_e2e_fixture.py" \
  --service asr --host 127.0.0.1 --port 18022 --mode normal \
  > "$GITHUB_WORKSPACE/mixed_asr_fixture.log" 2>&1 &
mixed_asr_pid=$!

for i in $(seq 1 15); do
  grep -q '"event": "listening"' "$GITHUB_WORKSPACE/mixed_tts_fixture.log" 2>/dev/null \
    && grep -q '"event": "listening"' "$GITHUB_WORKSPACE/mixed_asr_fixture.log" 2>/dev/null \
    && break
  sleep 1
done

cd "$package_dir"
stdbuf -oL -eL env LD_LIBRARY_PATH="$PACKAGE_LIB" \
  bin/unimrcpserver -r "$package_dir" -l 6 -o 1 -w \
  < /dev/null > "$GITHUB_WORKSPACE/mixed_server.log" 2>&1 &
mixed_server_pid=$!

mixed_ready=0
for i in $(seq 1 60); do
  if grep -q "Create MRCPv2 Profile" "$GITHUB_WORKSPACE/mixed_server.log" 2>/dev/null; then
    mixed_ready=1; break
  fi
  if ! kill -0 "$mixed_server_pid" 2>/dev/null; then break; fi
  sleep 1
done
if [ "$mixed_ready" != 1 ]; then
  echo "mixed_server did not become ready" >&2
  tail -n 120 "$GITHUB_WORKSPACE/mixed_server.log" >&2 || true
  exit 1
fi
sleep 2

cd "$GITHUB_WORKSPACE"
bash tools/stress/stress_test_improved.sh \
  -t mixed -c "$CONCURRENCY" -i 1 \
  -r "$GITHUB_WORKSPACE" \
  -a "$GITHUB_WORKSPACE/data" \
  --tts-save --tts-var-dir "$GITHUB_WORKSPACE/var"

kill "$mixed_server_pid" "$mixed_tts_pid" "$mixed_asr_pid" 2>/dev/null || true
for wait_i in 1 2 3 4 5; do
  kill -0 "$mixed_server_pid" 2>/dev/null \
    || kill -0 "$mixed_tts_pid" 2>/dev/null \
    || kill -0 "$mixed_asr_pid" 2>/dev/null \
    || break
  sleep 1
done
for _spid in "$mixed_server_pid" "$mixed_tts_pid" "$mixed_asr_pid"; do
  kill -0 "$_spid" 2>/dev/null && kill -9 "$_spid" 2>/dev/null || true
done
wait "$mixed_server_pid" "$mixed_tts_pid" "$mixed_asr_pid" 2>/dev/null || true
rm -f "$GITHUB_WORKSPACE/mixed_server.log"

# 12. Rapid successive session test (resource-cleanup and anti-deadlock gate)
#     rapid_stop_test.sh: 5 workers x 5 cycles of complete sessions back-to-back.
#     Validates no deadlock (timeout), no server crash, and no resource leak.
python3 "$GITHUB_WORKSPACE/tools/diagnostics/websocket_e2e_fixture.py" \
  --emit-config "$GITHUB_WORKSPACE/conf/unimrcpserver.xml" \
    "$GITHUB_WORKSPACE/rapid-unimrcpserver.xml" \
  --host 127.0.0.1 --tts-port 18091 --asr-port 18022
cp "$GITHUB_WORKSPACE/rapid-unimrcpserver.xml" "$package_dir/conf/unimrcpserver.xml"

python3 "$GITHUB_WORKSPACE/tools/diagnostics/websocket_e2e_fixture.py" \
  --service tts --host 127.0.0.1 --port 18091 --mode normal \
  > "$GITHUB_WORKSPACE/rapid_tts_fixture.log" 2>&1 &
rapid_tts_pid=$!

python3 "$GITHUB_WORKSPACE/tools/diagnostics/websocket_e2e_fixture.py" \
  --service asr --host 127.0.0.1 --port 18022 --mode normal \
  > "$GITHUB_WORKSPACE/rapid_asr_fixture.log" 2>&1 &
rapid_asr_pid=$!

for i in $(seq 1 15); do
  grep -q '"event": "listening"' "$GITHUB_WORKSPACE/rapid_tts_fixture.log" 2>/dev/null \
    && grep -q '"event": "listening"' "$GITHUB_WORKSPACE/rapid_asr_fixture.log" 2>/dev/null \
    && break
  sleep 1
done

cd "$package_dir"
stdbuf -oL -eL env LD_LIBRARY_PATH="$PACKAGE_LIB" \
  bin/unimrcpserver -r "$package_dir" -l 6 -o 1 -w \
  < /dev/null > "$GITHUB_WORKSPACE/rapid_server.log" 2>&1 &
rapid_server_pid=$!

rapid_ready=0
for i in $(seq 1 60); do
  if grep -q "Create MRCPv2 Profile" "$GITHUB_WORKSPACE/rapid_server.log" 2>/dev/null; then
    rapid_ready=1; break
  fi
  if ! kill -0 "$rapid_server_pid" 2>/dev/null; then break; fi
  sleep 1
done
if [ "$rapid_ready" != 1 ]; then
  echo "rapid_server did not become ready" >&2
  tail -n 120 "$GITHUB_WORKSPACE/rapid_server.log" >&2 || true
  exit 1
fi
sleep 2

cd "$GITHUB_WORKSPACE"
UMC_BIN="$GITHUB_WORKSPACE/platforms/umc/umc" \
SERVER_LOG="$GITHUB_WORKSPACE/rapid_server.log" \
  bash tools/stress/rapid_stop_test.sh \
    -t tts -c 5 -r "$GITHUB_WORKSPACE" --cycles=5
UMC_BIN="$GITHUB_WORKSPACE/platforms/umc/umc" \
SERVER_LOG="$GITHUB_WORKSPACE/rapid_server.log" \
  bash tools/stress/rapid_stop_test.sh \
    -t asr -c 5 -r "$GITHUB_WORKSPACE" --cycles=5 \
    -a "$GITHUB_WORKSPACE/data"

kill "$rapid_server_pid" "$rapid_tts_pid" "$rapid_asr_pid" 2>/dev/null || true
for wait_i in 1 2 3 4 5; do
  kill -0 "$rapid_server_pid" 2>/dev/null \
    || kill -0 "$rapid_tts_pid" 2>/dev/null \
    || kill -0 "$rapid_asr_pid" 2>/dev/null \
    || break
  sleep 1
done
for _spid in "$rapid_server_pid" "$rapid_tts_pid" "$rapid_asr_pid"; do
  kill -0 "$_spid" 2>/dev/null && kill -9 "$_spid" 2>/dev/null || true
done
wait "$rapid_server_pid" "$rapid_tts_pid" "$rapid_asr_pid" 2>/dev/null || true
rm -f "$GITHUB_WORKSPACE/rapid_server.log"

# 13. Production-plugin E2E: packaged TTS and ASR binaries/plugins
# exchange real WebSocket frames with loopback doubles.  This is
# intentionally separate from the pacing gate so a passing stress
# run cannot mask a missing TTS plugin or a bad MRCP completion.
# Exercise normal framing, short reads, delayed framing, and
# early remote connection closure.
for e2e_mode in normal split slow close; do
  E2E_WORK_DIR="$GITHUB_WORKSPACE/websocket-e2e-results/$e2e_mode" \
    PACKAGE_DIR="$package_dir" ROOT_DIR="$GITHUB_WORKSPACE" \
    E2E_MODE="$e2e_mode" \
    bash "$GITHUB_WORKSPACE/tools/diagnostics/run_websocket_e2e.sh"
done
CONTAINER_SCRIPT
