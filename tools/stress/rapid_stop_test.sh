#!/bin/bash
###############################################################################
# rapid_stop_test.sh — Rapid successive MRCP session cycle test
#
# 用途：验证 TTS/ASR 插件在高频率 MRCP 会话重建下的资源清理正确性。
# 每个 Worker 连续完成 CYCLES 轮完整会话（back-to-back），验证：
#   1. 无死锁（每轮在 TIMEOUT 内完成）
#   2. 无服务器崩溃（进程存活 + 日志无致命错误）
#   3. 无资源泄漏（多轮后成功率不退化）
#
# 用法：
#   rapid_stop_test.sh -t tts -c 5 -r /opt/unimrcp --cycles 5
#   rapid_stop_test.sh -t asr -c 5 -r /opt/unimrcp --cycles 5 -a /opt/unimrcp/data
#
# 参数：
#   -t  测试类型: tts | asr (默认: tts)
#   -c  并发 Worker 数 (默认: 5)
#   -r  UniMRCP 根目录 (默认: 脚本所在仓库根目录)
#   -a  ASR 音频源目录，type=asr 时需要 (默认: ROOT_DIR/data)
#   --cycles=N  每个 Worker 的连续循环轮数 (默认: 5)
#
# 环境变量：
#   SERVER_LOG  若设置，额外检查此日志文件中是否含崩溃信号
###############################################################################

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
TEST_TYPE="tts"
CONCURRENCY=5
CYCLES=5
AUDIO_DIR=""
TIMEOUT_PER_CYCLE=30  # 每轮最大秒数，超时视为死锁

# --------------------------------------------------------------------------
# 参数解析
# --------------------------------------------------------------------------
while getopts "t:c:r:a:h-:" opt; do
    case $opt in
        t) TEST_TYPE=$OPTARG ;;
        c) CONCURRENCY=$OPTARG ;;
        r) ROOT_DIR=$OPTARG ;;
        a) AUDIO_DIR=$OPTARG ;;
        h)
            sed -n '2,/^###/p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        -)
            case "$OPTARG" in
                cycles=*) CYCLES="${OPTARG#*=}" ;;
                timeout=*) TIMEOUT_PER_CYCLE="${OPTARG#*=}" ;;
                *) echo "Unknown option: --$OPTARG" >&2; exit 1 ;;
            esac
            ;;
        *) exit 1 ;;
    esac
done

# --------------------------------------------------------------------------
# 全局路径
# --------------------------------------------------------------------------
RESULT_DIR="$ROOT_DIR/rapid_stop_results"
EXP_SCRIPT="$ROOT_DIR/tests/integration/umc_test.exp"
STRESS_INPUT_PCM="$ROOT_DIR/data/stress_test_input.pcm"
UMC_BIN="${UMC_BIN:-$ROOT_DIR/platforms/umc/umc}"

if [[ -z "$AUDIO_DIR" ]]; then
    AUDIO_DIR="$ROOT_DIR/data"
fi

mkdir -p "$RESULT_DIR"

# --------------------------------------------------------------------------
# 检查前置条件
# --------------------------------------------------------------------------
if [[ ! -f "$EXP_SCRIPT" ]]; then
    echo "Error: expect script not found: $EXP_SCRIPT" >&2
    exit 1
fi

if [[ ! -x "$UMC_BIN" ]]; then
    echo "Error: umc binary not found or not executable: $UMC_BIN" >&2
    exit 1
fi

if [[ "$TEST_TYPE" == "asr" ]]; then
    SCENARIO="recog"
    # 为 ASR 准备共享音频文件
    audio_file=$(find "$AUDIO_DIR" -maxdepth 2 -name "*.pcm" -type f 2>/dev/null | head -1)
    if [[ -n "$audio_file" ]]; then
        cp "$audio_file" "$STRESS_INPUT_PCM"
    else
        echo "Warning: no .pcm audio file found in $AUDIO_DIR, ASR sessions may fail" >&2
    fi
elif [[ "$TEST_TYPE" == "tts" ]]; then
    SCENARIO="synth"
    audio_file=""
else
    echo "Error: -t must be tts or asr, got: $TEST_TYPE" >&2
    exit 1
fi

echo "===== Rapid session cycle test ====="
echo "Type:        $TEST_TYPE"
echo "Workers:     $CONCURRENCY"
echo "Cycles:      $CYCLES"
echo "Timeout/cycle: ${TIMEOUT_PER_CYCLE}s"
echo "Root:        $ROOT_DIR"
echo "====================================="

# --------------------------------------------------------------------------
# 准备 Worker 隔离环境（独立 SIP/RTP 端口）。umc_test.exp 根据 worker
# root 推导 UMC 路径，因此每个 worker 必须包含可执行文件、配置和数据。
# 端口分配与 stress_test_improved.sh 一致：
#   SIP  port = 8062 + worker_id
#   RTP  min  = 4000 + worker_id * 200
#   RTP  max  = RTP_min + 199
# --------------------------------------------------------------------------
for ((w = 0; w < CONCURRENCY; w++)); do
    wdir="$RESULT_DIR/worker_${w}"
    mkdir -p "$wdir/conf/umc-scenarios" "$wdir/conf/client-profiles" \
        "$wdir/data" "$wdir/var" "$wdir/platforms/umc"

    ln -sf "$UMC_BIN" "$wdir/platforms/umc/umc"
    for f in "$ROOT_DIR/conf/umc-scenarios/"* "$ROOT_DIR/conf/client-profiles/"*; do
        [[ -f "$f" ]] || continue
        ln -sf "$f" "$wdir/conf/$(basename "$(dirname "$f")")/$(basename "$f")"
    done
    for f in "$ROOT_DIR/data/"*; do
        [[ -f "$f" ]] || continue
        ln -sf "$f" "$wdir/data/$(basename "$f")"
    done

    sip_port=$((8062 + w))
    rtp_min=$((4000 + w * 200))
    rtp_max=$((rtp_min + 199))

    sed \
        -e "s|<sip-port>[0-9]*</sip-port>|<sip-port>${sip_port}</sip-port>|g" \
        -e "s|<rtp-port-min>[0-9]*</rtp-port-min>|<rtp-port-min>${rtp_min}</rtp-port-min>|g" \
        -e "s|<rtp-port-max>[0-9]*</rtp-port-max>|<rtp-port-max>${rtp_max}</rtp-port-max>|g" \
        "$ROOT_DIR/conf/unimrcpclient.xml" > "$wdir/conf/unimrcpclient.xml"

    if [[ "$TEST_TYPE" == "asr" ]] && [[ -n "${audio_file:-}" ]]; then
        mkdir -p "$wdir/data"
        cp "$audio_file" "$wdir/data/stress_test_input.pcm" || true
    fi
done

# --------------------------------------------------------------------------
# Worker 执行函数：连续完成 CYCLES 轮完整 MRCP 会话
# --------------------------------------------------------------------------
run_worker() {
    local worker_id=$1
    local wdir="$RESULT_DIR/worker_${worker_id}"
    local ok=0
    local failed=0
    local timeout_count=0

    for ((cycle = 1; cycle <= CYCLES; cycle++)); do
        local log="$wdir/cycle_${cycle}.log"

        local rc=0
        if timeout "$TIMEOUT_PER_CYCLE" \
            "$EXP_SCRIPT" "$SCENARIO" "uni2" "$wdir" "$worker_id" stop \
            > "$log" 2>&1; then
            rc=0
        else
            rc=$?
        fi

        if [[ $rc -eq 0 ]]; then
            if grep -q 'RESULT:STOPPED' "$log"; then
                ok=$((ok + 1))
            elif grep -Eq 'RESULT:(SUCCESS|MRCP_ERROR)' "$log" && \
                grep -Eq 'SPEAK-COMPLETE|RECOGNITION-COMPLETE' "$log"; then
                ok=$((ok + 1))
            else
                echo "FAIL: worker=$worker_id cycle=$cycle did not receive completion" >&2
                failed=$((failed + 1))
            fi
        elif [[ $rc -eq 124 ]]; then
            echo "TIMEOUT: worker=$worker_id cycle=$cycle (possible deadlock)" >&2
            timeout_count=$((timeout_count + 1))
            failed=$((failed + 1))
        else
            failed=$((failed + 1))
        fi
    done

    echo "Worker ${worker_id}: ${ok}/${CYCLES} passed (timeouts: ${timeout_count})"
    [[ $failed -eq 0 ]]
}

# --------------------------------------------------------------------------
# 并发启动所有 Worker
# --------------------------------------------------------------------------
pids=()
for ((w = 0; w < CONCURRENCY; w++)); do
    run_worker "$w" &
    pids+=($!)
done

total_failed=0
for pid in "${pids[@]}"; do
    wait "$pid" || total_failed=$((total_failed + 1))
done

# --------------------------------------------------------------------------
# 验证：服务器进程存活
# --------------------------------------------------------------------------
if command -v pgrep > /dev/null 2>&1; then
    if ! pgrep -f unimrcpserver > /dev/null 2>&1; then
        echo "FATAL: unimrcpserver appears to have died during rapid stop test" >&2
        exit 1
    fi
fi

# --------------------------------------------------------------------------
# 验证：服务器日志无崩溃信号
# --------------------------------------------------------------------------
if [[ -n "${SERVER_LOG:-}" ]] && [[ -f "$SERVER_LOG" ]]; then
    if grep -qiE \
        "(Segmentation fault|SIGSEGV|SIGABRT|double free|stack smash|corrupted)" \
        "$SERVER_LOG" 2>/dev/null; then
        echo "FATAL: crash signal found in server log" >&2
        grep -iE \
            "(Segmentation fault|SIGSEGV|SIGABRT|double free|stack smash|corrupted)" \
            "$SERVER_LOG" >&2 || true
        exit 1
    fi
fi

# --------------------------------------------------------------------------
# 清理 Worker 环境
# --------------------------------------------------------------------------
rm -rf "$RESULT_DIR"/worker_* 2>/dev/null || true

# --------------------------------------------------------------------------
# 最终结果
# --------------------------------------------------------------------------
if [[ $total_failed -gt 0 ]]; then
    echo "FAIL: ${total_failed}/${CONCURRENCY} workers had failures" >&2
    exit 1
fi

echo "PASS: rapid stop test complete (type=${TEST_TYPE} workers=${CONCURRENCY} cycles=${CYCLES})"
