#!/bin/bash

###############################################################################
# UniMRCP ASR/TTS 压力测试脚本 (改进版)
#
# 服务器上执行：每轮 20 并发，共 10 轮
#./tools/stress/stress_test_improved.sh -t tts -c 20 -i 10 -r /home/apps/tte-mrcp

# ASR + TTS 全测：每轮 10 并发，共 5 轮，轮间等 2 秒
#./tools/stress/stress_test_improved.sh -t all -c 10 -i 5 -r /home/apps/tte-mrcp -a /home/apps/tte-mrcp/data -d 2
# 使用方法：
#   ./tools/stress/stress_test_improved.sh -c 5 -t tts -i 10
#   ./tools/stress/stress_test_improved.sh -t asr -a /path/to/audio -o results.csv
#   ./tools/stress/stress_test_improved.sh -t all -i 20 -a /path/to/audio --tts-save
###############################################################################

CONCURRENCY=1
TEST_TYPE="all"
ITERATIONS=10
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
AUDIO_DIR=""                # ASR 音频源目录
OUTPUT_CSV=""               # ASR 结果输出 CSV 文件
SAMPLE_RATE=16000           # 默认采样率
TTS_SAVE=false              # 是否保留每次的 TTS 录音
TTS_VAR_DIR=""              # TTS 录音输出目录（默认 ROOT_DIR/var）
ROUND_DELAY=0               # 轮间延迟（秒），默认不延迟

# 解析参数
while getopts "c:t:i:r:a:o:s:d:h-:" opt; do
    case $opt in
        c) CONCURRENCY=$OPTARG ;;
        t) TEST_TYPE=$OPTARG ;;
        i) ITERATIONS=$OPTARG ;;
        r) ROOT_DIR=$OPTARG ;;
        a) AUDIO_DIR=$OPTARG ;;
        o) OUTPUT_CSV=$OPTARG ;;
        s) SAMPLE_RATE=$OPTARG ;;
        d) ROUND_DELAY=$OPTARG ;;
        h)
            echo "用法: $0 [-c 并发数] [-t 类型(asr/tts/all)] [-i 轮数] [-r 根目录]"
            echo "              [-a ASR音频目录] [-o ASR结果CSV] [-s 采样率] [-d 轮间延迟秒数]"
            echo "              [--tts-save] [--tts-var-dir 目录] [--round-delay=秒数]"
            echo ""
            echo "参数说明:"
            echo "  -c  并发数，每轮同时发起的请求数 (默认: 1)"
            echo "  -t  测试类型: asr | tts | all (默认: all)"
            echo "  -i  轮数，总共执行多少轮并发 (默认: 10)"
            echo "  -r  UniMRCP 根目录 (默认: 脚本所在仓库根目录)"
            echo "  -a  ASR 音频源目录，包含 .pcm 文件 (默认: ROOT_DIR/data)"
            echo "  -o  ASR 结果输出 CSV 文件 (默认: ROOT_DIR/stress_results/asr_results.csv)"
            echo "  -s  音频采样率: 8000 | 16000 (默认: 16000)"
            echo "  -d  每轮之间的等待秒数 (默认: 0，不等待)"
            echo "  --tts-save        保留每次 TTS 生成的录音 (默认每次测试前清空)"
            echo "  --tts-var-dir     TTS 录音输出目录 (默认: ROOT_DIR/var)"
            echo "  --round-delay=秒数 每轮之间的等待秒数 (同 -d)"
            echo "  -h                显示此帮助信息"
            echo ""
            echo "===== 常用示例 ====="
            echo ""
            echo "  # 只发 1 次 ASR 请求（单次测试）"
            echo "  $0 -t asr -c 1 -i 1 -r /home/apps/tte-mrcp -a /home/apps/tte-mrcp/data"
            echo ""
            echo "  # 只发 1 次 TTS 请求（单次测试）"
            echo "  $0 -t tts -c 1 -i 1 -r /home/apps/tte-mrcp"
            echo ""
            echo "  # 并发 10 个 ASR 请求（1 轮 = 同时发 10 个）"
            echo "  $0 -t asr -c 10 -i 1 -r /home/apps/tte-mrcp -a /home/apps/tte-mrcp/data"
            echo ""
            echo "  # 并发 10 个 TTS 请求（1 轮）"
            echo "  $0 -t tts -c 10 -i 1 -r /home/apps/tte-mrcp"
            echo ""
            echo "  # ASR 压测：每轮 20 并发，共 10 轮（总计 200 次请求），轮间等 2 秒"
            echo "  $0 -t asr -c 20 -i 10 -d 2 -r /home/apps/tte-mrcp -a /home/apps/tte-mrcp/data"
            echo ""
            echo "  # TTS 压测：每轮 10 并发，共 10 轮"
            echo "  $0 -t tts -c 10 -i 10 -r /home/apps/tte-mrcp"
            echo ""
            echo "  # 指定自定义音频目录和结果输出文件"
            echo "  $0 -t asr -c 5 -i 10 -r /home/apps/tte-mrcp -a /home/apps/tts_server/var -o ./my_result.csv"
            echo ""
            echo "===== 概念说明 ====="
            echo ""
            echo "  单次请求:  -c 1 -i 1        (并发=1, 轮数=1, 总计 1 次)"
            echo "  并发请求:  -c N -i 1        (N 个请求同时发出, 总计 N 次)"
            echo "  压力测试:  -c N -i M        (每轮 N 并发, 共 M 轮, 总计 N×M 次)"
            echo "  轮间延迟:  -d S             (每轮之间等待 S 秒, 避免瞬间压垮服务器)"
            echo ""
            echo "  并发 vs 串行:"
            echo "    -c 1 -i 100    串行发 100 次, 一个接一个"
            echo "    -c 100 -i 1    一次性并发 100 个请求"
            echo "    -c 10 -i 10    分 10 轮, 每轮 10 个并发, 总计 100 次"
            exit 0
            ;;
        -)
            case "$OPTARG" in
                tts-save)
                    TTS_SAVE=true
                    ;;
                tts-var-dir=*)
                    TTS_VAR_DIR="${OPTARG#*=}"
                    ;;
                round-delay=*)
                    ROUND_DELAY="${OPTARG#*=}"
                    ;;
                *)
                    echo "未知选项: --$OPTARG"
                    exit 1
                    ;;
            esac
            ;;
    esac
done

# 设置默认值
if [[ -z "$AUDIO_DIR" ]]; then
    AUDIO_DIR="$ROOT_DIR/data"
fi
if [[ -z "$OUTPUT_CSV" ]]; then
    OUTPUT_CSV="$ROOT_DIR/stress_results/asr_results.csv"
fi
if [[ -z "$TTS_VAR_DIR" ]]; then
    TTS_VAR_DIR="$ROOT_DIR/var"
fi

UMC_BIN="$ROOT_DIR/platforms/umc/umc"
RESULT_DIR="$ROOT_DIR/stress_results"
EXP_SCRIPT="$ROOT_DIR/tests/integration/umc_test.exp"
RECOG_SCENARIO_XML="$ROOT_DIR/conf/umc-scenarios/recognizer.xml"
STRESS_INPUT_PCM="$ROOT_DIR/data/stress_test_input.pcm"

mkdir -p "$RESULT_DIR"
mkdir -p "$TTS_VAR_DIR"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo "=========================================="
echo "UniMRCP 压力测试 (改进版)"
echo "=========================================="
echo "并发数:     $CONCURRENCY (每轮同时发起的请求数)"
echo "测试类型:   $TEST_TYPE"
echo "轮数:       $ITERATIONS"
echo "根目录:     $ROOT_DIR"
if [[ "$ROUND_DELAY" -gt 0 ]]; then
    echo "轮间延迟:   ${ROUND_DELAY}s"
fi
if [[ "$TEST_TYPE" == "asr" || "$TEST_TYPE" == "all" ]]; then
    echo "ASR 音频源: $AUDIO_DIR"
    echo "ASR 结果:   $OUTPUT_CSV"
    echo "采样率:     ${SAMPLE_RATE}Hz"
fi
if [[ "$TEST_TYPE" == "tts" || "$TEST_TYPE" == "all" ]]; then
    echo "TTS 输出:   $TTS_VAR_DIR"
    echo "保留录音:   $([ "$TTS_SAVE" = true ] && echo '是' || echo '否 (每次清空)')"
fi
echo "=========================================="

# ============================================================================
# 工具函数
# ============================================================================

# 检测音频采样率 (从文件名推断)
detect_sample_rate() {
    local fname
    fname=$(basename "$1")
    if echo "$fname" | grep -qi "16k"; then
        echo 16000
    elif echo "$fname" | grep -qi "8k"; then
        echo 8000
    elif echo "$fname" | grep -qi "8000"; then
        echo 8000
    elif echo "$fname" | grep -qi "16000"; then
        echo 16000
    else
        echo "$SAMPLE_RATE"
    fi
}

# 获取音频文件列表
get_audio_files() {
    local dir="$1"
    local __var="$2"
    local __files=()
    if [[ ! -d "$dir" ]]; then
        echo -e "${RED}✗ 音频目录不存在: $dir${NC}"
        return 1
    fi
    # 读取 .pcm 文件到数组
    while IFS= read -r -d '' file; do
        __files+=("$file")
    done < <(find "$dir" -maxdepth 1 -name "*.pcm" -type f -print0 | sort -z)
    if [[ ${#__files[@]} -eq 0 ]]; then
        echo -e "${RED}✗ 音频目录中没有 .pcm 文件: $dir${NC}"
        return 1
    fi
    echo -e "${CYAN}✓ 找到 ${#__files[@]} 个音频文件${NC}"
    for f in "${__files[@]}"; do
        echo "    - $(basename "$f")"
    done
    # 使用 eval 将数组返回到调用者变量（兼容 bash < 4.3，避免 local -n nameref）
    eval "$__var=(\"\${__files[@]}\")"
    return 0
}

# 解析 ASR 日志中的识别结果
parse_recognition_result() {
    local log_file="$1"
    local result=""

    # 防御：日志文件不存在则直接返回
    if [[ ! -f "$log_file" ]]; then
        echo "(日志文件不存在)"
        return
    fi

    # 方式0: 从 MRCP 消息的 NLSML XML body 中提取识别文本（最可靠，始终存在）
    # 日志格式（MRCP 协议使用 CRLF 换行，日志中每行末尾有 \r 即 ^M）：
    #   <result>                          ← 外层根标签，单独一行，无文本内容
    #     <instance>
    #       <result>实际识别文本</result>  ← 内层 <result>，单行包含完整文本
    #     </instance>
    #
    # 关键：先用 tr -d '\r' 去除所有 ^M，避免 grep/sed 匹配到 \r 而非实际文本。
    # grep -o 提取匹配的 <result>...</result> 对；[^<]* 只匹配不含 < 的文本，
    # 这确保了只匹配内层 <result>（含实际文本），不会误匹配外层 <result>（无文本）。
    result=$(tr -d '\r' < "$log_file" 2>/dev/null | grep -o '<result>[^<]*</result>' 2>/dev/null | head -1 | sed 's/<result>//;s/<\/result>//' | tr -d '\n')

    # 方式1: 从 expect 脚本 Phase 2 捕获的 Interpretation[].instance[] trace 提取
    # 格式: Interpretation[0].instance[0]: <result>识别文本</result>
    # 或:   Interpretation[0].instance[0]: 识别文本  (无 XML 包裹时)
    if [[ -z "$result" ]]; then
        result=$(tr -d '\r' < "$log_file" 2>/dev/null | grep 'Interpretation\[' 2>/dev/null | grep '\.instance\[' 2>/dev/null | head -1 | sed 's/.*\.instance\[[0-9]*\]: //' | tr -d '\n')
        # 如果 trace 包含 XML 包裹的 <result> 标签，解包
        if [[ "$result" == "<result>"*"</result>" ]]; then
            result=$(echo "$result" | sed 's/<result>//;s/<\/result>//')
        fi
    fi

    # 方式2: 从 expect 脚本输出的 NLSML: 标记提取（格式: RESULT:SUCCESS NLSML:<text>）
    if [[ -z "$result" ]]; then
        result=$(tr -d '\r' < "$log_file" 2>/dev/null | grep 'NLSML:' 2>/dev/null | head -1 | sed 's/.*NLSML://' | tr -d '\n')
        # 同样尝试解包 XML 包裹
        if [[ "$result" == "<result>"*"</result>" ]]; then
            result=$(echo "$result" | sed 's/<result>//;s/<\/result>//')
        fi
    fi

    # 截断过长的结果
    if [[ ${#result} -gt 500 ]]; then
        result="${result:0:500}..."
    fi

    # 如果仍然为空，返回明确标记
    if [[ -z "$result" ]]; then
        result="(无识别文本)"
    fi

    echo "$result"
}

# 获取测试状态
get_test_status() {
    local log_file="$1"

    # 优先检查 SIP/基础设施级致命错误
    if grep -q "RESULT:SIP_ERROR\|Failed to Create NUA\|initializing SIP stack failed\|Address already in use\|Failed to Run Sofia-SIP" "$log_file" 2>/dev/null; then
        echo "SIP_ERROR"
    # 检查 MRCP 协议层错误（Completion-Cause 非零）
    elif grep -q "RESULT:MRCP_ERROR\|Completion-Cause:\[[:space:]]*00[1-9]" "$log_file" 2>/dev/null; then
        echo "MRCP_ERROR"
    elif grep -q "RESULT:SUCCESS" "$log_file" 2>/dev/null; then
        echo "SUCCESS"
    elif grep -q "RESULT:TIMEOUT" "$log_file" 2>/dev/null; then
        echo "TIMEOUT"
    elif grep -q "RESULT:NODATA" "$log_file" 2>/dev/null; then
        echo "NODATA"
    elif grep -q "RECOGNITION-COMPLETE\|SPEAK-COMPLETE" "$log_file" 2>/dev/null; then
        echo "SUCCESS"
    elif grep -qi "error\|fail" "$log_file" 2>/dev/null; then
        echo "ERROR"
    else
        echo "UNKNOWN"
    fi
}

# 获取测试耗时（秒）
get_test_duration() {
    local log_file="$1"
    local duration=""
    # 尝试从日志中提取耗时信息
    duration=$(grep -oP 'duration[:\s]+\K[0-9.]+' "$log_file" 2>/dev/null | head -1)
    if [[ -z "$duration" ]]; then
        # 尝试从 timestamp 差值计算
        local start_time end_time
        start_time=$(head -1 "$log_file" 2>/dev/null | grep -oP '\d{2}:\d{2}:\d{2}' | head -1)
        end_time=$(tail -1 "$log_file" 2>/dev/null | grep -oP '\d{2}:\d{2}:\d{2}' | head -1)
        duration="N/A"
    fi
    echo "${duration:-N/A}"
}

# ============================================================================
# 环境检查
# ============================================================================

check_environment() {
    echo ""
    echo "检查环境..."

    # 检查服务器
    if ! pgrep -f "unimrcpserver" > /dev/null; then
        echo -e "${RED}✗ UniMRCP 服务器未运行！${NC}"
        echo "  请启动: cd $ROOT_DIR && ./platforms/unimrcp-server/unimrcpserver -r . -l 4 -o 3"
        return 1
    fi
    echo -e "${GREEN}✓ 服务器运行中${NC}"

    # 检查 umc 工具
    if [[ ! -x "$UMC_BIN" ]]; then
        echo -e "${RED}✗ umc 工具不存在: $UMC_BIN${NC}"
        return 1
    fi
    echo -e "${GREEN}✓ umc 工具就绪${NC}"

    # 检查 expect 脚本
    if [[ ! -f "$EXP_SCRIPT" ]]; then
        echo -e "${RED}✗ expect 脚本不存在: $EXP_SCRIPT${NC}"
        return 1
    fi
    echo -e "${GREEN}✓ expect 脚本就绪${NC}"


    return 0
}

# ============================================================================
# 为每个 worker 创建隔离的运行环境（独立 SIP/RTP 端口，避免冲突）
# ============================================================================

prepare_worker_envs() {
    local max_workers="${1:-1}"
    local sip_base=8062
    local rtp_base=4000
    local rtp_range=200

    echo "准备 worker 隔离环境 (最多 $max_workers 个并发)..."
    for ((w=0; w<max_workers; w++)); do
        local wroot="$RESULT_DIR/worker_${w}"
        if [[ -d "$wroot/conf" ]]; then
            continue  # 已存在，跳过
        fi

        mkdir -p "$wroot/conf/umc-scenarios" \
                 "$wroot/conf/client-profiles" \
                 "$wroot/data" \
                 "$wroot/var" \
                 "$wroot/platforms/umc"

        # 软链 umc 二进制（库路径由 wrapper 内的绝对路径保证）
        ln -sf "$ROOT_DIR/platforms/umc/umc" "$wroot/platforms/umc/umc" 2>/dev/null

        # 软链 data 目录中的音频文件
        for f in "$ROOT_DIR/data/"*; do
            [[ -f "$f" ]] && ln -sf "$f" "$wroot/data/$(basename "$f")" 2>/dev/null
        done

        # 软链 conf 子目录
        for d in umc-scenarios client-profiles; do
            if [[ -d "$ROOT_DIR/conf/$d" ]]; then
                for f in "$ROOT_DIR/conf/$d/"*; do
                    [[ -f "$f" ]] && ln -sf "$f" "$wroot/conf/$d/$(basename "$f")" 2>/dev/null
                done
            fi
        done

        # 软链 conf 下的其他文件（除 unircpclient.xml 需要修改端口）
        for f in "$ROOT_DIR/conf/"*; do
            [[ ! -f "$f" ]] && continue
            local bn
            bn=$(basename "$f")
            if [[ "$bn" == "unimrcpclient.xml" ]]; then
                # 生成带有独立端口的配置
                local sip_port=$((sip_base + w))
                local rtp_min=$((rtp_base + w * rtp_range))
                local rtp_max=$((rtp_min + rtp_range - 1))
                sed -e "s|<sip-port>[0-9]*</sip-port>|<sip-port>$sip_port</sip-port>|" \
                    -e "s|<rtp-port-min>[0-9]*</rtp-port-min>|<rtp-port-min>$rtp_min</rtp-port-min>|" \
                    -e "s|<rtp-port-max>[0-9]*</rtp-port-max>|<rtp-port-max>$rtp_max</rtp-port-max>|" \
                    "$ROOT_DIR/conf/$bn" > "$wroot/conf/$bn"
            else
                ln -sf "$f" "$wroot/conf/$bn" 2>/dev/null
            fi
        done

        echo "  worker-$w: SIP=${sip_port}, RTP=${rtp_min}-${rtp_max}"
    done
    echo "Worker 环境准备完成"
}

cleanup_worker_envs() {
    echo "清理 worker 环境..."
    rm -rf "$RESULT_DIR"/worker_* 2>/dev/null
    echo "清理完成"
}

# ============================================================================
# 运行单个测试
# ============================================================================

run_test() {
    local test_type="$1"
    local test_id="$2"
    local worker_id="${3:-0}"
    local log_file="$RESULT_DIR/${test_type}_${test_id}.log"

    if [[ ! -f "$EXP_SCRIPT" ]]; then
        echo "Error: expect script not found: $EXP_SCRIPT"
        return 1
    fi

    local cmd_type="synth"
    if [[ "$test_type" == "asr" ]]; then
        cmd_type="recog"
    fi

    # 记录测试开始时间
    echo "===== $(date '+%Y-%m-%d %H:%M:%S') =====" > "$log_file"
    echo "[worker-$worker_id] SIP port: $((8062 + worker_id))" >> "$log_file"

    # 使用 worker 隔离环境（独立 SIP/RTP 端口）
    local worker_root="$RESULT_DIR/worker_${worker_id}"
    if [[ ! -d "$worker_root" ]]; then
        worker_root="$ROOT_DIR"  # 降级：使用默认根目录
    fi
    "$EXP_SCRIPT" "$cmd_type" "uni2" "$worker_root" "$worker_id" >> "$log_file" 2>&1
    local result=$?

    # 记录测试结束时间
    echo "===== $(date '+%Y-%m-%d %H:%M:%S') =====" >> "$log_file"

    # 检查结果
    local status
    status=$(get_test_status "$log_file")
    local recog_text=""

    case "$status" in
        SUCCESS)
            echo -e "[${test_type}-${test_id}] ${GREEN}✓ 成功${NC}"
            if [[ "$test_type" == "asr" ]]; then
                recog_text=$(parse_recognition_result "$log_file")
                if [[ -n "$recog_text" ]]; then
                    echo -e "         识别: ${recog_text:0:100}"
                fi
            fi
            return 0
            ;;
        MRCP_ERROR)
            local mrcp_cause
            mrcp_cause=$(grep -oP 'Completion-Cause:\s*\K.*' "$log_file" 2>/dev/null | head -1 | tr -d '\r\n')
            echo -e "[${test_type}-${test_id}] ${RED}✗ MRCP错误${NC} - ${mrcp_cause:-Completion-Cause error}"
            return 1
            ;;
        SIP_ERROR)
            echo -e "[${test_type}-${test_id}] ${RED}✗ SIP错误${NC} - 端口冲突或SIP栈初始化失败"
            return 1
            ;;
        TIMEOUT)
            echo -e "[${test_type}-${test_id}] ${YELLOW}✗ 超时${NC}"
            return 1
            ;;
        *)
            local error_msg
            error_msg=$(grep -i "error\|fail\|timeout\|Completion-Cause" "$log_file" 2>/dev/null | head -3 | tr '\n' ' ')
            echo -e "[${test_type}-${test_id}] ${RED}✗ 失败 ($status)${NC} - ${error_msg:-无详细信息}"
            return 1
            ;;
    esac
}

# ============================================================================
# 主测试流程
# ============================================================================

main() {
    check_environment || exit 1

    local total_tests=$((CONCURRENCY * ITERATIONS))
    local current_test=0
    local success_count=0
    local fail_count=0

    # 并发控制
    local -a pids=()
    local worker exit_code

    # ASR 相关变量
    local -a asr_audio_files=()
    local asr_file_count=0
    local audio_file audio_name audio_rate file_index
    local test_label log_file status recog_text escaped_text timestamp

    # TTS 相关变量
    local tts_audio_before=0
    local tts_audio_after=0

    echo ""
    echo "开始测试..."
    echo ""

    # 为并发 worker 准备隔离环境（独立 SIP/RTP 端口）
    if [[ "$CONCURRENCY" -gt 1 ]]; then
        prepare_worker_envs "$CONCURRENCY"
        echo ""
    fi

    # ========================================================================
    # TTS 测试
    # ========================================================================
    if [[ "$TEST_TYPE" == "tts" || "$TEST_TYPE" == "all" ]]; then
        echo -e "${CYAN}--- TTS 测试 ---${NC}"

        # TTS 录音目录管理
        mkdir -p "$TTS_VAR_DIR"
        if [[ "$TTS_SAVE" != true ]]; then
            # 清空之前的 TTS 录音
            rm -f "$TTS_VAR_DIR"/synth-*.pcm 2>/dev/null
            echo "已清空 TTS 录音目录: $TTS_VAR_DIR"
        fi
        tts_audio_before=$(find "$TTS_VAR_DIR" -maxdepth 1 -name "synth-*.pcm" -type f 2>/dev/null | wc -l | tr -d ' ')
        echo "TTS 录音输出目录: $TTS_VAR_DIR (已有 ${tts_audio_before} 个录音文件)"
        echo ""

        for ((iter=1; iter<=ITERATIONS; iter++)); do
            echo -e "${CYAN}--- TTS 第 $iter/$ITERATIONS 轮 (${CONCURRENCY} 并发) ---${NC}"

            pids=()
            # 同时启动所有并发 worker
            for ((worker=0; worker<CONCURRENCY; worker++)); do
                ((current_test++))
                run_test "tts" "${worker}_${iter}" "$worker" >/dev/null &
                pids+=($!)
            done

            # 等待本轮所有 worker 完成并统计结果
            for pid in "${pids[@]}"; do
                wait "$pid"
                if [[ $? -eq 0 ]]; then
                    ((success_count++))
                else
                    ((fail_count++))
                fi
            done

            echo -e "${GREEN}  第 $iter 轮完成${NC}"

            # 轮间延迟
            if [[ "$ROUND_DELAY" -gt 0 ]] && [[ $iter -lt $ITERATIONS ]]; then
                sleep "$ROUND_DELAY"
            fi
        done
        echo ""

        # 从 worker 隔离环境中收集 synth-*.pcm 文件到 TTS_VAR_DIR
        # worker 隔离时 umc 以 worker_root 运行，synth-*.pcm 写入 worker 的 var/ 而非 ROOT_DIR/var/
        if [[ "$CONCURRENCY" -gt 1 ]]; then
            for ((w=0; w<CONCURRENCY; w++)); do
                local wvar="$RESULT_DIR/worker_${w}/var"
                if [[ -d "$wvar" ]]; then
                    for f in "$wvar"/synth-*.pcm; do
                        [[ -f "$f" ]] && cp "$f" "$TTS_VAR_DIR/" 2>/dev/null
                    done
                fi
            done
        fi

        # 统计 TTS 生成的文件
        tts_audio_after=$(find "$TTS_VAR_DIR" -maxdepth 1 -name "synth-*.pcm" -type f 2>/dev/null | wc -l | tr -d ' ')
        local tts_new=$((tts_audio_after - tts_audio_before))
        echo -e "${GREEN}TTS 测试完成，本次生成 ${tts_new} 个录音文件${NC}"
        if [[ "$tts_new" -gt 0 ]]; then
            echo "录音文件列表:"
            find "$TTS_VAR_DIR" -maxdepth 1 -name "synth-*.pcm" -type f -newer "$RESULT_DIR" 2>/dev/null | sort | while read -r f; do
                local fsize
                fsize=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f" 2>/dev/null)
                echo "    $(basename "$f") (${fsize} bytes)"
            done
        fi
        echo ""
    fi

    # ========================================================================
    # ASR 测试
    # ========================================================================
    if [[ "$TEST_TYPE" == "asr" || "$TEST_TYPE" == "all" ]]; then
        echo -e "${CYAN}--- ASR 测试 ---${NC}"

        # 加载音频文件列表
        if ! get_audio_files "$AUDIO_DIR" asr_audio_files; then
            echo -e "${RED}无法加载音频文件，ASR 测试跳过${NC}"
        else
            asr_file_count=${#asr_audio_files[@]}
            echo "ASR 结果将写入: $OUTPUT_CSV"
            echo ""

            # 初始化 CSV 文件（写入表头）
            mkdir -p "$(dirname "$OUTPUT_CSV")"
            echo "timestamp,iteration,audio_file,sample_rate,status,recognized_text,log_file" > "$OUTPUT_CSV"

            for ((iter=1; iter<=ITERATIONS; iter++)); do
                # 轮询选择音频文件（每轮所有并发 worker 使用同一音频）
                file_index=$(( (iter - 1) % asr_file_count ))
                audio_file="${asr_audio_files[$file_index]}"
                audio_name=$(basename "$audio_file")
                audio_rate=$(detect_sample_rate "$audio_file")

                # 复制音频文件到主 data 目录
                if ! cp "$audio_file" "$STRESS_INPUT_PCM"; then
                    echo -e "${RED}✗ 音频文件复制失败: $audio_file -> $STRESS_INPUT_PCM${NC}"
                    exit 1
                fi
                # 同步到所有 worker 隔离环境的 data 目录（确保 umc 以 worker_root 运行时也能找到）
                for ((w=0; w<CONCURRENCY; w++)); do
                    wdata="$RESULT_DIR/worker_${w}/data"
                    if [[ -d "$wdata" ]]; then
                        cp "$audio_file" "$wdata/stress_test_input.pcm" || {
                            echo -e "${RED}✗ 音频文件复制到 worker-${w} 失败${NC}"
                            exit 1
                        }
                    fi
                done

                timestamp=$(date '+%Y-%m-%d %H:%M:%S')
                echo -e "${CYAN}--- ASR 第 $iter/$ITERATIONS 轮 (${CONCURRENCY} 并发) 音频: ${YELLOW}$audio_name${NC} (${audio_rate}Hz) ---${NC}"

                pids=()
                # 同时启动所有并发 worker
                for ((worker=0; worker<CONCURRENCY; worker++)); do
                    ((current_test++))
                    run_test "asr" "${worker}_${iter}" "$worker" >/dev/null &
                    pids+=($!)
                done

                # 等待本轮所有 worker 完成并收集结果
                for ((i=0; i<${#pids[@]}; i++)); do
                    worker=$i
                    wait "${pids[$i]}"
                    exit_code=$?

                    test_label="${worker}_${iter}"
                    log_file="$RESULT_DIR/asr_${test_label}.log"
                    status=$(get_test_status "$log_file")
                    recog_text=""

                    if [[ $exit_code -eq 0 ]]; then
                        ((success_count++))
                    else
                        ((fail_count++))
                    fi

                    # 无论测试成功/失败，都尝试从日志中提取识别结果
                    # （即使 MRCP 返回错误，日志中也可能有部分识别文本）
                    recog_text=$(parse_recognition_result "$log_file")

                    # 写入 CSV 结果（转义特殊字符）
                    escaped_text="${recog_text//\"/\"\"}"
                    echo "$timestamp,$iter,$audio_name,$audio_rate,$status,\"$escaped_text\",$log_file" >> "$OUTPUT_CSV"
                done

                echo -e "${GREEN}  第 $iter 轮完成${NC}"

                # 轮间延迟
                if [[ "$ROUND_DELAY" -gt 0 ]] && [[ $iter -lt $ITERATIONS ]]; then
                    sleep "$ROUND_DELAY"
                fi
            done
            echo ""

            # 清理临时文件
            rm -f "$STRESS_INPUT_PCM"

            echo -e "${GREEN}ASR 测试完成，结果已写入: $OUTPUT_CSV${NC}"
        fi
        echo ""
    fi

    # ========================================================================
    # 统计结果
    # ========================================================================
    echo "=========================================="
    echo "测试结果汇总"
    echo "=========================================="

    local total=0
    local success=0
    local fail=0
    local sip_error=0
    local mrcp_error=0
    local timeout=0
    local nodata=0
    local unknown=0

    for log in "$RESULT_DIR"/tts_*.log "$RESULT_DIR"/asr_*.log; do
        if [[ -f "$log" ]]; then
            ((total++))
            local st
            st=$(get_test_status "$log")
            case "$st" in
                SUCCESS) ((success++)) ;;
                SIP_ERROR) ((fail++)); ((sip_error++)) ;;
                MRCP_ERROR) ((fail++)); ((mrcp_error++)) ;;
                TIMEOUT) ((fail++)); ((timeout++)) ;;
                NODATA) ((fail++)); ((nodata++)) ;;
                *) ((fail++)); ((unknown++)) ;;
            esac
        fi
    done

    if [[ $total -gt 0 ]]; then
        local rate=$((success * 100 / total))
        echo "总测试: $total"
        echo -e "成功:   ${GREEN}$success${NC} (${rate}%)"
        echo -e "失败:   ${RED}$fail${NC}"
        if [[ $sip_error -gt 0 ]]; then
            echo -e "        └─ SIP端口冲突 (SIP_ERROR): ${RED}$sip_error${NC}"
        fi
        if [[ $mrcp_error -gt 0 ]]; then
            echo -e "        └─ 服务器MRCP错误 (MRCP_ERROR/004): ${RED}$mrcp_error${NC}"
        fi
        if [[ $timeout -gt 0 ]]; then
            echo -e "        └─ 超时 (TIMEOUT): ${YELLOW}$timeout${NC}"
        fi
        if [[ $nodata -gt 0 ]]; then
            echo -e "        └─ 无数据 (NODATA): ${YELLOW}$nodata${NC}"
        fi
        if [[ $unknown -gt 0 ]]; then
            echo -e "        └─ 未知错误 (UNKNOWN): ${YELLOW}$unknown${NC}"
        fi
    else
        echo "没有找到测试日志"
    fi

    echo ""
    echo "详细日志: $RESULT_DIR"
    if [[ -f "$OUTPUT_CSV" ]]; then
        echo "ASR 结果 CSV: $OUTPUT_CSV"
        # 显示 CSV 摘要
        local csv_lines
        csv_lines=$(tail -n +2 "$OUTPUT_CSV" 2>/dev/null | wc -l | tr -d ' ')
        echo "  CSV 记录数: $csv_lines"
    fi
    if [[ -d "$TTS_VAR_DIR" ]]; then
        local tts_count
        tts_count=$(find "$TTS_VAR_DIR" -maxdepth 1 -name "synth-*.pcm" -type f 2>/dev/null | wc -l | tr -d ' ')
        echo "TTS 录音目录: $TTS_VAR_DIR (${tts_count} 个文件)"
    fi
    echo ""
    echo "提示: 查看失败原因: grep -i error $RESULT_DIR/*.log"

    # 清理 worker 隔离环境
    if [[ "$CONCURRENCY" -gt 1 ]]; then
        cleanup_worker_envs
    fi
}

main
