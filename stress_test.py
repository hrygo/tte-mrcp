#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
UniMRCP ASR/TTS 压力测试脚本 (Python版本)

功能:
    - 并发压测ASR（语音识别）功能
    - 并发压测TTS（语音合成）功能
    - 支持指定并发数目
    - 统计成功/失败次数和响应时间

使用方法:
    python3 stress_test.py [-c CONCURRENCY] [-t TEST_TYPE] [-i ITERATIONS] [-r ROOT_DIR]

示例:
    # 单进程测试ASR
    python3 stress_test.py -t asr

    # 10并发测试TTS
    python3 stress_test.py -c 10 -t tts

    # 5并发测试ASR和TTS，各20次
    python3 stress_test.py -c 5 -i 20
"""

import argparse
import os
import sys
import time
import subprocess
import threading
import re
from datetime import datetime
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Tuple, List, Dict

# 颜色输出
class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    BLUE = '\033[0;34m'
    NC = '\033[0m'  # No Color


class Logger:
    """简单的日志类"""

    @staticmethod
    def info(msg: str):
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        print(f"{Colors.GREEN}[INFO]{Colors.NC} [{timestamp}] {msg}")

    @staticmethod
    def warn(msg: str):
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        print(f"{Colors.YELLOW}[WARN]{Colors.NC} [{timestamp}] {msg}")

    @staticmethod
    def error(msg: str):
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        print(f"{Colors.RED}[ERROR]{Colors.NC} [{timestamp}] {msg}")

    @staticmethod
    def debug(msg: str):
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        print(f"{Colors.BLUE}[DEBUG]{Colors.NC} [{timestamp}] {msg}")


class StressTestConfig:
    """压力测试配置"""

    def __init__(self):
        self.concurrency = 1
        self.test_type = "all"
        self.iterations = 10
        self.root_dir = os.path.dirname(os.path.abspath(__file__))
        self.profile = "uni2"

        # 派生路径
        self.umc_bin = os.path.join(self.root_dir, "platforms/umc/umc")
        self.conf_dir = os.path.join(self.root_dir, "conf")
        self.data_dir = os.path.join(self.root_dir, "data")
        self.scenario_dir = os.path.join(self.conf_dir, "umc-scenarios")
        self.result_dir = os.path.join(self.root_dir, "stress_results")


class TestResult:
    """单次测试结果"""

    def __init__(self):
        self.success = False
        self.duration = 0  # 毫秒
        self.error = None


class StressTestRunner:
    """压力测试运行器"""

    def __init__(self, config: StressTestConfig):
        self.config = config
        self.asr_success = 0
        self.asr_fail = 0
        self.tts_success = 0
        self.tts_fail = 0
        self.total_asr_time = 0
        self.total_tts_time = 0
        self.lock = threading.Lock()

        # 创建结果目录
        os.makedirs(self.config.result_dir, exist_ok=True)

    def check_environment(self) -> bool:
        """检查测试环境"""
        Logger.info("检查测试环境...")

        # 检查umc二进制文件
        if not os.path.isfile(self.config.umc_bin):
            Logger.error(f"找不到umc工具: {self.config.umc_bin}")
            Logger.error("请先编译项目")
            return False
        Logger.debug(f"找到umc工具: {self.config.umc_bin}")

        # 检查配置文件
        if not os.path.isdir(self.config.conf_dir):
            Logger.error(f"找不到配置目录: {self.config.conf_dir}")
            return False

        # 检查场景文件
        synth_scenario = os.path.join(self.config.scenario_dir, "synthesizer.xml")
        recog_scenario = os.path.join(self.config.scenario_dir, "recognizer.xml")

        if self.config.test_type in ["tts", "all"] and not os.path.isfile(synth_scenario):
            Logger.error(f"找不到TTS场景文件: {synth_scenario}")
            return False

        if self.config.test_type in ["asr", "all"] and not os.path.isfile(recog_scenario):
            Logger.error(f"找不到ASR场景文件: {recog_scenario}")
            return False

        # 检查speak.xml
        speak_file = os.path.join(self.config.data_dir, "speak.xml")
        if not os.path.isfile(speak_file):
            Logger.warn(f"找不到speak.xml，将创建默认文件: {speak_file}")
            with open(speak_file, 'w', encoding='utf-8') as f:
                f.write('<?xml version="1.0"?>\n<speak>\n你好，今天天气怎么样\n</speak>\n')

        Logger.info("环境检查通过")
        return True

    def run_asr_once(self, worker_id: int, iteration: int) -> TestResult:
        """运行单次ASR测试"""
        result = TestResult()
        start_time = time.time()

        Logger.debug(f"ASR Worker-{worker_id}: 开始第 {iteration} 次测试")

        output_file = os.path.join(
            self.config.result_dir,
            f"asr_worker_{worker_id}_iter_{iteration}.log"
        )

        try:
            # 运行umc ASR测试
            process = subprocess.Popen(
                [self.config.umc_bin, "-r", self.config.root_dir, "-l", "4", "-o", "1"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True
            )

            # 发送命令
            process.stdin.write(f"run recog {self.config.profile}\n")
            process.stdin.flush()
            time.sleep(3)  # 等待测试完成

            process.stdin.write("exit\n")
            process.stdin.flush()

            # 获取输出并设置超时
            try:
                output, _ = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()

            # 保存输出
            with open(output_file, 'w', encoding='utf-8') as f:
                f.write(output)

            # 检查结果
            if "RECOGNITION-COMPLETE" in output:
                if "completion-cause=000" in output or "Completion-Cause: Success" in output:
                    result.success = True
                    result.duration = int((time.time() - start_time) * 1000)
                    Logger.debug(f"ASR Worker-{worker_id}: 第 {iteration} 次测试成功 ({result.duration}ms)")
                else:
                    result.success = False
                    result.duration = int((time.time() - start_time) * 1000)
                    result.error = "Recognition failed"
                    Logger.debug(f"ASR Worker-{worker_id}: 第 {iteration} 次测试失败")
            else:
                result.success = False
                result.duration = int((time.time() - start_time) * 1000)
                result.error = "No RECOGNITION-COMPLETE found"
                Logger.debug(f"ASR Worker-{worker_id}: 第 {iteration} 次测试失败")

        except Exception as e:
            result.success = False
            result.duration = int((time.time() - start_time) * 1000)
            result.error = str(e)
            Logger.error(f"ASR Worker-{worker_id}: 第 {iteration} 次测试异常: {e}")

        return result

    def run_tts_once(self, worker_id: int, iteration: int) -> TestResult:
        """运行单次TTS测试"""
        result = TestResult()
        start_time = time.time()

        Logger.debug(f"TTS Worker-{worker_id}: 开始第 {iteration} 次测试")

        output_file = os.path.join(
            self.config.result_dir,
            f"tts_worker_{worker_id}_iter_{iteration}.log"
        )

        try:
            # 运行umc TTS测试
            process = subprocess.Popen(
                [self.config.umc_bin, "-r", self.config.root_dir, "-l", "4", "-o", "1"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True
            )

            # 发送命令
            process.stdin.write(f"run synth {self.config.profile}\n")
            process.stdin.flush()
            time.sleep(5)  # 等待测试完成

            process.stdin.write("exit\n")
            process.stdin.flush()

            # 获取输出并设置超时
            try:
                output, _ = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()

            # 保存输出
            with open(output_file, 'w', encoding='utf-8') as f:
                f.write(output)

            # 检查结果
            if "SPEAK-COMPLETE" in output or "Speak-Complete" in output:
                result.success = True
                result.duration = int((time.time() - start_time) * 1000)
                Logger.debug(f"TTS Worker-{worker_id}: 第 {iteration} 次测试成功 ({result.duration}ms)")
            else:
                result.success = False
                result.duration = int((time.time() - start_time) * 1000)
                result.error = "No SPEAK-COMPLETE found"
                Logger.debug(f"TTS Worker-{worker_id}: 第 {iteration} 次测试失败")

        except Exception as e:
            result.success = False
            result.duration = int((time.time() - start_time) * 1000)
            result.error = str(e)
            Logger.error(f"TTS Worker-{worker_id}: 第 {iteration} 次测试异常: {e}")

        return result

    def asr_worker(self, worker_id: int, iterations: int):
        """ASR Worker进程"""
        success = 0
        fail = 0
        total_time = 0

        Logger.info(f"ASR Worker-{worker_id}: 启动 (迭代次数: {iterations})")

        for i in range(1, iterations + 1):
            result = self.run_asr_once(worker_id, i)

            with self.lock:
                if result.success:
                    success += 1
                    total_time += result.duration
                else:
                    fail += 1

            # 避免过快请求
            time.sleep(1)

        Logger.info(f"ASR Worker-{worker_id}: 完成 (成功: {success}, 失败: {fail})")

        return success, fail, total_time

    def tts_worker(self, worker_id: int, iterations: int):
        """TTS Worker进程"""
        success = 0
        fail = 0
        total_time = 0

        Logger.info(f"TTS Worker-{worker_id}: 启动 (迭代次数: {iterations})")

        for i in range(1, iterations + 1):
            result = self.run_tts_once(worker_id, i)

            with self.lock:
                if result.success:
                    success += 1
                    total_time += result.duration
                else:
                    fail += 1

            # 避免过快请求
            time.sleep(1)

        Logger.info(f"TTS Worker-{worker_id}: 完成 (成功: {success}, 失败: {fail})")

        return success, fail, total_time

    def run_test(self):
        """运行压力测试"""
        Logger.info("=" * 40)
        Logger.info("开始压力测试")
        Logger.info("=" * 40)
        Logger.info(f"测试类型: {self.config.test_type}")
        Logger.info(f"并发数目: {self.config.concurrency}")
        Logger.info(f"迭代次数: {self.config.iterations}")
        Logger.info(f"MRCP配置: {self.config.profile}")
        Logger.info("=" * 40)

        # 清空结果目录
        for f in os.listdir(self.config.result_dir):
            if f.endswith('.log'):
                os.remove(os.path.join(self.config.result_dir, f))

        # 使用线程池并发执行
        with ThreadPoolExecutor(max_workers=self.config.concurrency * 2) as executor:
            futures = {}
            future_list = []

            # 启动ASR Worker
            if self.config.test_type in ["asr", "all"]:
                Logger.info(f"启动 {self.config.concurrency} 个ASR Worker进程...")
                for i in range(1, self.config.concurrency + 1):
                    future = executor.submit(self.asr_worker, i, self.config.iterations)
                    futures[future] = "asr"
                    future_list.append(future)

            # 启动TTS Worker
            if self.config.test_type in ["tts", "all"]:
                Logger.info(f"启动 {self.config.concurrency} 个TTS Worker进程...")
                for i in range(1, self.config.concurrency + 1):
                    future = executor.submit(self.tts_worker, i, self.config.iterations)
                    futures[future] = "tts"
                    future_list.append(future)

            # 收集结果
            for future in as_completed(future_list):
                try:
                    success, fail, total_time = future.result()
                    test_type = futures.get(future, "")

                    if test_type == "asr":
                        self.asr_success += success
                        self.asr_fail += fail
                        self.total_asr_time += total_time
                    elif test_type == "tts":
                        self.tts_success += success
                        self.tts_fail += fail
                        self.total_tts_time += total_time

                except Exception as e:
                    Logger.error(f"Worker执行异常: {e}")

        # 打印统计结果
        self.print_summary()

    def print_summary(self):
        """打印测试摘要"""
        print()
        print("=" * 40)
        print("           测试结果摘要")
        print("=" * 40)
        print(f"测试类型: {self.config.test_type}")
        print(f"并发数目: {self.config.concurrency}")
        print(f"迭代次数: {self.config.iterations}")
        print()

        # ASR统计
        if self.config.test_type in ["asr", "all"]:
            total_asr = self.asr_success + self.asr_fail
            asr_avg_time = self.total_asr_time // self.asr_success if self.asr_success > 0 else 0
            asr_rate = (self.asr_success / total_asr * 100) if total_asr > 0 else 0

            print("------ ASR (语音识别) ------")
            print(f"  总请求数: {total_asr}")
            print(f"  成功数量: {Colors.GREEN}{self.asr_success}{Colors.NC}")
            print(f"  失败数量: {Colors.RED}{self.asr_fail}{Colors.NC}")
            print(f"  成功率:   {asr_rate:.2f}%")
            if self.asr_success > 0:
                print(f"  平均响应时间: {asr_avg_time}ms")
            print()

        # TTS统计
        if self.config.test_type in ["tts", "all"]:
            total_tts = self.tts_success + self.tts_fail
            tts_avg_time = self.total_tts_time // self.tts_success if self.tts_success > 0 else 0
            tts_rate = (self.tts_success / total_tts * 100) if total_tts > 0 else 0

            print("------ TTS (语音合成) ------")
            print(f"  总请求数: {total_tts}")
            print(f"  成功数量: {Colors.GREEN}{self.tts_success}{Colors.NC}")
            print(f"  失败数量: {Colors.RED}{self.tts_fail}{Colors.NC}")
            print(f"  成功率:   {tts_rate:.2f}%")
            if self.tts_success > 0:
                print(f"  平均响应时间: {tts_avg_time}ms")
            print()

        print("=" * 40)
        print(f"详细日志位置: {self.config.result_dir}")
        print("=" * 40)


def main():
    """主函数"""
    parser = argparse.ArgumentParser(
        description="UniMRCP ASR/TTS 压力测试脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s -t asr                    # 单进程测试ASR
  %(prog)s -c 10 -t tts              # 10并发测试TTS
  %(prog)s -c 5 -i 20                # 5并发测试ASR和TTS，各20次
        """
    )

    parser.add_argument(
        "-c", "--concurrency",
        type=int,
        default=1,
        help="并发数目 (默认: 1)"
    )
    parser.add_argument(
        "-t", "--test-type",
        choices=["asr", "tts", "all"],
        default="all",
        help="测试类型 (默认: all)"
    )
    parser.add_argument(
        "-i", "--iterations",
        type=int,
        default=10,
        help="每个进程的迭代次数 (默认: 10)"
    )
    parser.add_argument(
        "-r", "--root-dir",
        default=os.path.dirname(os.path.abspath(__file__)),
        help="项目根目录"
    )
    parser.add_argument(
        "-p", "--profile",
        default="uni2",
        help="MRCP配置文件 (默认: uni2)"
    )

    args = parser.parse_args()

    # 创建配置
    config = StressTestConfig()
    config.concurrency = args.concurrency
    config.test_type = args.test_type
    config.iterations = args.iterations
    config.root_dir = args.root_dir
    config.profile = args.profile

    # 更新派生路径
    config.umc_bin = os.path.join(config.root_dir, "platforms/umc/umc")
    config.conf_dir = os.path.join(config.root_dir, "conf")
    config.data_dir = os.path.join(config.root_dir, "data")
    config.scenario_dir = os.path.join(config.conf_dir, "umc-scenarios")
    config.result_dir = os.path.join(config.root_dir, "stress_results")

    # 创建测试运行器
    runner = StressTestRunner(config)

    # 检查环境
    if not runner.check_environment():
        sys.exit(1)

    # 运行测试
    try:
        runner.run_test()
    except KeyboardInterrupt:
        Logger.warn("\n测试被用户中断")
        runner.print_summary()
        sys.exit(1)


if __name__ == "__main__":
    main()
