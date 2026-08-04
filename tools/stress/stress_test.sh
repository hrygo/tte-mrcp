#!/bin/bash

###############################################################################
# UniMRCP ASR/TTS 压力测试脚本
#
# 功能：
#   - 并发压测ASR（语音识别）功能
#   - 并发压测TTS（语音合成）功能
#   - 支持指定并发数目
#   - 统计成功/失败次数和响应时间
#
# 使用方法：
#   ./tools/stress/stress_test.sh [-c concurrency] [-t test_type] [-h]
###############################################################################

# 默认参数
CONCURRENCY=1
TEST_TYPE="all"
ITERATIONS=10
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
UMC_BIN="$ROOT_DIR/platforms/umc/umc"
CONF_DIR="$ROOT_DIR/conf"
DATA_DIR="$ROOT_DIR/data"
SCENARIO_DIR="$CONF_DIR/umc-scenarios"
RESULT_DIR="$ROOT_DIR/stress_results"
PROFILE="uni2"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# PID数组
declare -a PIDS=()

# 临时文件
TMP_DIR="/tmp/unimrcp_stress_test_$$"
mkdir -p "$TMP_DIR"

###############################################################################
# 函数：显示帮助信息
###############################################################################
usage() {
    cat << EOF
UniMRCP ASR/TTS 压力测试脚本

使用方法:
    $0 [options]

参数:
    -c concurrency   并发数目 (默认: 1)
    -t test_type     测试类型: asr|tts|all (默认: all)
    -i iterations    每个进程的迭代次数 (默认: 10)
    -r root_dir      项目根目录
    -p profile       MRCP配置文件 (默认: uni2)
    -h               显示帮助信息

示例:
    # 单进程测试ASR
    $0 -t asr

    # 10并发测试TTS
    $0 -c 10 -t tts

    # 5并发测试ASR和TTS，各20次
    $0 -c 5 -i 20

EOF
    exit 0
}

###############################################################################
# 函数：打印日志
###############################################################################
log() {
    local level=$1
    shift
    local msg="$@"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    case $level in
        INFO)  echo -e "${GREEN}[INFO]${NC} [$timestamp] $msg" ;;
        WARN)  echo -e "${YELLOW}[WARN]${NC} [$timestamp] $msg" ;;
        ERROR) echo -e "${RED}[ERROR]${NC} [$timestamp] $msg" ;;
        DEBUG) [[ $DEBUG -eq 1 ]] && echo -e "${BLUE}[DEBUG]${NC} [$timestamp] $msg" ;;
    esac
}

###############################################################################
# 函数：检查环境
###############################################################################
check_environment() {
    log INFO "检查测试环境..."

    if [[ ! -f "$UMC_BIN" ]]; then
        log ERROR "找不到umc工具: $UMC_BIN"
        exit 1
    fi

    if [[ ! -d "$CONF_DIR" ]]; then
        log ERROR "找不到配置目录: $CONF_DIR"
        exit 1
    fi

    # 创建结果目录
    mkdir -p "$RESULT_DIR"

    # 检查speak.xml
    if [[ ! -f "$DATA_DIR/speak.xml" ]]; then
        cat > "$DATA_DIR/speak.xml" << 'EOF'
<?xml version="1.0"?>
<speak>
你好，今天天气怎么样
</speak>
EOF
    fi

    log INFO "环境检查通过"
}

###############################################################################
# 函数：ASR单次测试
###############################################################################
run_asr_once() {
    local worker_id=$1
    local iteration=$2
    local output_file="$RESULT_DIR/asr_worker_${worker_id}_iter_${iteration}.log"
    local cmd_file="$TMP_DIR/asr_cmd_${worker_id}_${iteration}.txt"

    # 创建命令文件
    cat > "$cmd_file" << EOF
run recog $PROFILE
exit
EOF

    log DEBUG "ASR Worker-$worker_id: 开始第 $iteration 次测试"

    # 运行umc并记录时间
    local start_time=$(date +%s%N 2>/dev/null || echo "$(date +%s)000000000")

    "$UMC_BIN" -r "$ROOT_DIR" -l 4 -o 1 < "$cmd_file" > "$output_file" 2>&1 &
    local umc_pid=$!

    # 等待最多30秒
    local count=0
    while kill -0 $umc_pid 2>/dev/null; do
        if [[ $count -ge 30 ]]; then
            kill $umc_pid 2>/dev/null
            break
        fi
        sleep 1
        ((count++))
    done
    wait $umc_pid 2>/dev/null

    local end_time=$(date +%s%N 2>/dev/null || echo "$(date +%s)000000000")
    local duration=$(( (end_time - start_time) / 1000000 ))

    # 检查结果
    if grep -q "RECOGNITION-COMPLETE" "$output_file" 2>/dev/null; then
        if grep -q "completion-cause=000" "$output_file" 2>/dev/null || \
           grep -q "Completion-Cause: Success" "$output_file" 2>/dev/null || \
           grep -q "cause=000" "$output_file" 2>/dev/null; then
            echo "SUCCESS,$duration" >> "$TMP_DIR/asr_worker_${worker_id}_result.txt"
            log DEBUG "ASR Worker-$worker_id: 第 $iteration 次测试成功 (${duration}ms)"
            return 0
        fi
    fi

    echo "FAIL,$duration" >> "$TMP_DIR/asr_worker_${worker_id}_result.txt"
    log DEBUG "ASR Worker-$worker_id: 第 $iteration 次测试失败 (${duration}ms)"
    return 1
}

###############################################################################
# 函数：TTS单次测试
###############################################################################
run_tts_once() {
    local worker_id=$1
    local iteration=$2
    local output_file="$RESULT_DIR/tts_worker_${worker_id}_iter_${iteration}.log"
    local cmd_file="$TMP_DIR/tts_cmd_${worker_id}_${iteration}.txt"

    # 创建命令文件
    cat > "$cmd_file" << EOF
run synth $PROFILE
exit
EOF

    log DEBUG "TTS Worker-$worker_id: 开始第 $iteration 次测试"

    # 运行umc并记录时间
    local start_time=$(date +%s%N 2>/dev/null || echo "$(date +%s)000000000")

    "$UMC_BIN" -r "$ROOT_DIR" -l 4 -o 1 < "$cmd_file" > "$output_file" 2>&1 &
    local umc_pid=$!

    # 等待最多30秒
    local count=0
    while kill -0 $umc_pid 2>/dev/null; do
        if [[ $count -ge 30 ]]; then
            kill $umc_pid 2>/dev/null
            break
        fi
        sleep 1
        ((count++))
    done
    wait $umc_pid 2>/dev/null

    local end_time=$(date +%s%N 2>/dev/null || echo "$(date +%s)000000000")
    local duration=$(( (end_time - start_time) / 1000000 ))

    # 检查结果
    if grep -q "SPEAK-COMPLETE" "$output_file" 2>/dev/null || \
       grep -q "Speak-Complete" "$output_file" 2>/dev/null; then
        echo "SUCCESS,$duration" >> "$TMP_DIR/tts_worker_${worker_id}_result.txt"
        log DEBUG "TTS Worker-$worker_id: 第 $iteration 次测试成功 (${duration}ms)"
        return 0
    fi

    echo "FAIL,$duration" >> "$TMP_DIR/tts_worker_${worker_id}_result.txt"
    log DEBUG "TTS Worker-$worker_id: 第 $iteration 次测试失败 (${duration}ms)"
    return 1
}

###############################################################################
# 函数：ASR Worker进程
###############################################################################
asr_worker() {
    local worker_id=$1
    local iterations=$2
    local success=0
    local fail=0

    log INFO "ASR Worker-$worker_id: 启动 (迭代次数: $iterations)"

    > "$TMP_DIR/asr_worker_${worker_id}_result.txt"

    for ((i=1; i<=iterations; i++)); do
        if run_asr_once $worker_id $i; then
            ((success++))
        else
            ((fail++))
        fi
        sleep 1
    done

    log INFO "ASR Worker-$worker_id: 完成 (成功: $success, 失败: $fail)"
    echo "$success,$fail,0" > "$TMP_DIR/asr_worker_${worker_id}_stats.txt"
}

###############################################################################
# 函数：TTS Worker进程
###############################################################################
tts_worker() {
    local worker_id=$1
    local iterations=$2
    local success=0
    local fail=0

    log INFO "TTS Worker-$worker_id: 启动 (迭代次数: $iterations)"

    > "$TMP_DIR/tts_worker_${worker_id}_result.txt"

    for ((i=1; i<=iterations; i++)); do
        if run_tts_once $worker_id $i; then
            ((success++))
        else
            ((fail++))
        fi
        sleep 1
    done

    log INFO "TTS Worker-$worker_id: 完成 (成功: $success, 失败: $fail)"
    echo "$success,$fail,0" > "$TMP_DIR/tts_worker_${worker_id}_stats.txt"
}

###############################################################################
# 函数：统计结果
###############################################################################
collect_results() {
    log INFO "========================================"
    log INFO "收集测试结果..."
    log INFO "========================================"

    for pid in "${PIDS[@]}"; do
        wait $pid 2>/dev/null
    done

    ASR_SUCCESS=0
    ASR_FAIL=0
    TTS_SUCCESS=0
    TTS_FAIL=0

    # 汇总ASR结果
    if [[ "$TEST_TYPE" == "asr" || "$TEST_TYPE" == "all" ]]; then
        for file in "$TMP_DIR"/asr_worker_*_result.txt; do
            if [[ -f "$file" ]]; then
                while IFS=',' read -r status duration; do
                    if [[ "$status" == "SUCCESS" ]]; then
                        ((ASR_SUCCESS++))
                    else
                        ((ASR_FAIL++))
                    fi
                done < "$file"
            fi
        done
    fi

    # 汇总TTS结果
    if [[ "$TEST_TYPE" == "tts" || "$TEST_TYPE" == "all" ]]; then
        for file in "$TMP_DIR"/tts_worker_*_result.txt; do
            if [[ -f "$file" ]]; then
                while IFS=',' read -r status duration; do
                    if [[ "$status" == "SUCCESS" ]]; then
                        ((TTS_SUCCESS++))
                    else
                        ((TTS_FAIL++))
                    fi
                done < "$file"
            fi
        done
    fi

    print_summary
}

###############################################################################
# 函数：打印测试摘要
###############################################################################
print_summary() {
    local total_asr=$((ASR_SUCCESS + ASR_FAIL))
    local total_tts=$((TTS_SUCCESS + TTS_FAIL))

    echo ""
    echo "========================================"
    echo "           测试结果摘要"
    echo "========================================"
    echo "测试类型: $TEST_TYPE"
    echo "并发数目: $CONCURRENCY"
    echo "迭代次数: $ITERATIONS"
    echo ""

    if [[ "$TEST_TYPE" == "asr" || "$TEST_TYPE" == "all" ]]; then
        echo "------ ASR (语音识别) ------"
        echo "  总请求数: $total_asr"
        echo -e "  成功数量: ${GREEN}$ASR_SUCCESS${NC}"
        echo -e "  失败数量: ${RED}$ASR_FAIL${NC}"
        if [[ $total_asr -gt 0 ]]; then
            local asr_rate=$(awk "BEGIN {printf \"%.2f\", ($ASR_SUCCESS/$total_asr)*100}")
            echo "  成功率:   $asr_rate%"
        fi
        echo ""
    fi

    if [[ "$TEST_TYPE" == "tts" || "$TEST_TYPE" == "all" ]]; then
        echo "------ TTS (语音合成) ------"
        echo "  总请求数: $total_tts"
        echo -e "  成功数量: ${GREEN}$TTS_SUCCESS${NC}"
        echo -e "  失败数量: ${RED}$TTS_FAIL${NC}"
        if [[ $total_tts -gt 0 ]]; then
            local tts_rate=$(awk "BEGIN {printf \"%.2f\", ($TTS_SUCCESS/$total_tts)*100}")
            echo "  成功率:   $tts_rate%"
        fi
        echo ""
    fi

    echo "========================================"
    echo "详细日志位置: $RESULT_DIR"
    echo "========================================"
}

###############################################################################
# 函数：清理函数
###############################################################################
cleanup() {
    log INFO "收到中断信号，正在停止所有测试..."
    for pid in "${PIDS[@]}"; do
        kill $pid 2>/dev/null
    done
    collect_results
    rm -rf "$TMP_DIR"
    exit 1
}

###############################################################################
# 主函数
###############################################################################
main() {
    while getopts "c:t:i:r:p:h" opt; do
        case $opt in
            c) CONCURRENCY=$OPTARG ;;
            t) TEST_TYPE=$OPTARG ;;
            i) ITERATIONS=$OPTARG ;;
            r)
                ROOT_DIR=$OPTARG
                UMC_BIN="$ROOT_DIR/platforms/umc/umc"
                CONF_DIR="$ROOT_DIR/conf"
                DATA_DIR="$ROOT_DIR/data"
                SCENARIO_DIR="$CONF_DIR/umc-scenarios"
                RESULT_DIR="$ROOT_DIR/stress_results"
                ;;
            p) PROFILE=$OPTARG ;;
            h) usage ;;
            *) usage ;;
        esac
    done

    if [[ ! "$TEST_TYPE" =~ ^(asr|tts|all)$ ]]; then
        log ERROR "无效的测试类型: $TEST_TYPE"
        exit 1
    fi

    trap cleanup SIGINT SIGTERM

    check_environment

    rm -f "$RESULT_DIR"/*.log

    log INFO "========================================"
    log INFO "开始压力测试"
    log INFO "========================================"
    log INFO "测试类型: $TEST_TYPE"
    log INFO "并发数目: $CONCURRENCY"
    log INFO "迭代次数: $ITERATIONS"
    log INFO "MRCP配置: $PROFILE"
    log INFO "========================================"

    # 启动worker进程
    if [[ "$TEST_TYPE" == "asr" || "$TEST_TYPE" == "all" ]]; then
        log INFO "启动 $CONCURRENCY 个ASR Worker进程..."
        for ((i=1; i<=CONCURRENCY; i++)); do
            asr_worker $i $ITERATIONS &
            PIDS+=($!)
        done
    fi

    if [[ "$TEST_TYPE" == "tts" || "$TEST_TYPE" == "all" ]]; then
        log INFO "启动 $CONCURRENCY 个TTS Worker进程..."
        for ((i=1; i<=CONCURRENCY; i++)); do
            tts_worker $i $ITERATIONS &
            PIDS+=($!)
        done
    fi

    collect_results
    rm -rf "$TMP_DIR"
}

main "$@"
