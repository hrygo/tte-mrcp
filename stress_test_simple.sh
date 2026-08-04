#!/bin/bash

###############################################################################
# UniMRCP 简化压力测试脚本
#
# 使用方法：
#   ./stress_test_simple.sh -c 5 -t tts -i 10
###############################################################################

CONCURRENCY=1
TEST_TYPE="all"
ITERATIONS=10
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 解析参数
while getopts "c:t:i:r:h" opt; do
    case $opt in
        c) CONCURRENCY=$OPTARG ;;
        t) TEST_TYPE=$OPTARG ;;
        i) ITERATIONS=$OPTARG ;;
        r) ROOT_DIR=$OPTARG ;;
        h)
            echo "用法: $0 [-c 并发数] [-t 类型(asr/tts/all)] [-i 迭代次数] [-r 根目录]"
            exit 0
            ;;
    esac
done

UMC_BIN="$ROOT_DIR/platforms/umc/umc"
RESULT_DIR="$ROOT_DIR/stress_results"
mkdir -p "$RESULT_DIR"

echo "=========================================="
echo "UniMRCP 压力测试"
echo "=========================================="
echo "并发数: $CONCURRENCY"
echo "测试类型: $TEST_TYPE"
echo "迭代次数: $ITERATIONS"
echo "=========================================="

# 检查服务器是否运行
if ! pgrep -f "unimrcpserver" > /dev/null; then
    echo "错误: UniMRCP 服务器未运行！"
    echo "请先启动服务器: cd $ROOT_DIR && ./platforms/unimrcp-server/unimrcpserver conf/unimrcpserver.xml"
    exit 1
fi

echo "服务器状态: 运行中 ✓"
echo ""

# 统计变量
ASR_SUCCESS=0
ASR_FAIL=0
TTS_SUCCESS=0
TTS_FAIL=0

# 测试函数
test_asr() {
    local id=$1
    local log="$RESULT_DIR/asr_$id.log"

    # 使用expect脚本
    cat > /tmp/umc_asr_$$.exp << 'EXP_EOF'
#!/usr/bin/expect -f
set timeout 25
set umc [lindex $argv 0]
set root [lindex $argv 1]
spawn $umc -r $root -l 4 -o 1
expect ">*"
send "run recog uni2\r"
expect {
    "RECOGNITION-COMPLETE" { puts "SUCCESS"; exp_continue }
    timeout { puts "FAIL" }
}
expect ">*"
send "exit\r"
expect eof
EXP_EOF

    chmod +x /tmp/umc_asr_$$.exp
    /tmp/umc_asr_$$.exp "$UMC_BIN" "$ROOT_DIR" > "$log" 2>&1
    rm -f /tmp/umc_asr_$$.exp

    if grep -q "SUCCESS" "$log"; then
        echo "ASR-$id: ✓ 成功"
        return 0
    else
        echo "ASR-$id: ✗ 失败"
        return 1
    fi
}

test_tts() {
    local id=$1
    local log="$RESULT_DIR/tts_$id.log"

    # 使用expect脚本
    cat > /tmp/umc_tts_$$.exp << 'EXP_EOF'
#!/usr/bin/expect -f
set timeout 25
set umc [lindex $argv 0]
set root [lindex $argv 1]
spawn $umc -r $root -l 4 -o 1
expect ">*"
send "run synth uni2\r"
expect {
    "SPEAK-COMPLETE" { puts "SUCCESS"; exp_continue }
    timeout { puts "FAIL" }
}
expect ">*"
send "exit\r"
expect eof
EXP_EOF

    chmod +x /tmp/umc_tts_$$.exp
    /tmp/umc_tts_$$.exp "$UMC_BIN" "$ROOT_DIR" > "$log" 2>&1
    rm -f /tmp/umc_tts_$$.exp

    if grep -q "SUCCESS" "$log"; then
        echo "TTS-$id: ✓ 成功"
        return 0
    else
        echo "TTS-$id: ✗ 失败"
        return 1
    fi
}

# 运行测试
TOTAL_TESTS=$((CONCURRENCY * ITERATIONS))
CURRENT_TEST=0

if [[ "$TEST_TYPE" == "asr" || "$TEST_TYPE" == "all" ]]; then
    echo "开始 ASR 测试..."
    for ((iter=1; iter<=ITERATIONS; iter++)); do
        for ((worker=1; worker<=CONCURRENCY; worker++)); do
            ((CURRENT_TEST++))
            echo -ne "\r进度: [$CURRENT_TEST/$TOTAL_TESTS] "

            test_asr "${worker}_${iter}" &
            sleep 0.5  # 避免过快启动
        done
        wait  # 等待当前批次完成
    done
    echo ""
fi

if [[ "$TEST_TYPE" == "tts" || "$TEST_TYPE" == "all" ]]; then
    echo "开始 TTS 测试..."
    for ((iter=1; iter<=ITERATIONS; iter++)); do
        for ((worker=1; worker<=CONCURRENCY; worker++)); do
            ((CURRENT_TEST++))
            echo -ne "\r进度: [$CURRENT_TEST/$TOTAL_TESTS] "

            test_tts "${worker}_${iter}" &
            sleep 0.5
        done
        wait
    done
    echo ""
fi

# 统计结果
echo ""
echo "=========================================="
echo "统计结果"
echo "=========================================="

for log in "$RESULT_DIR"/asr_*.log; do
    if [[ -f "$log" ]]; then
        if grep -q "SUCCESS" "$log"; then
            ((ASR_SUCCESS++))
        else
            ((ASR_FAIL++))
        fi
    fi
done

for log in "$RESULT_DIR"/tts_*.log; do
    if [[ -f "$log" ]]; then
        if grep -q "SUCCESS" "$log"; then
            ((TTS_SUCCESS++))
        else
            ((TTS_FAIL++))
        fi
    fi
done

if [[ "$TEST_TYPE" == "asr" || "$TEST_TYPE" == "all" ]]; then
    echo "ASR: 成功 $ASR_SUCCESS, 失败 $ASR_FAIL"
fi

if [[ "$TEST_TYPE" == "tts" || "$TEST_TYPE" == "all" ]]; then
    echo "TTS: 成功 $TTS_SUCCESS, 失败 $TTS_FAIL"
fi

echo "=========================================="
echo "详细日志: $RESULT_DIR"
