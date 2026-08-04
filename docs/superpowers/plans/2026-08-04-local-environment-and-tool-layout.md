# TTE-MRCP Local Environment and Tool Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 补充本机环境事实说明，并将压测、诊断和集成脚本按职责归位，同时保持根目录命令兼容。

**Architecture:** 运行时源码目录不变；工具资产分成 `tools/stress/`、`tools/diagnostics/` 和 `tests/integration/`，根目录脚本变为最小兼容包装。README 记录环境能力和依赖边界，避免把本机验证误写成通用构建结论。

**Tech Stack:** Bash, Python 3, Expect, Markdown, Git.

## Global Constraints

- 以当前脚本和构建配置为 SSOT；不依据旧文档臆测路径。
- 保留现有用户源码修改，不改动 `plugins/*/src/*.c` 的业务逻辑。
- 所有移动可恢复，所有 shell 命令使用 `rtk` 前缀，文件编辑使用 `apply_patch`。

### Task 1: 建立职责目录并移动工具

**Files:**
- Create: `tools/stress/`, `tools/diagnostics/`, `tests/integration/`
- Move: root stress scripts → `tools/stress/`
- Move: `diagnose.sh` → `tools/diagnostics/diagnose.sh`
- Move: `umc_test.exp` → `tests/integration/umc_test.exp`

- [x] **Step 1:** 创建目标目录。
- [x] **Step 2:** 移动脚本并保留执行权限。
- [x] **Step 3:** 在根目录创建兼容包装脚本，包装使用自身路径计算仓库根目录并 `exec` 新脚本。

### Task 2: 修正脚本路径和本机默认值

**Files:**
- Modify: `tools/stress/stress_test_improved.sh`
- Modify: `tools/stress/stress_test.py`
- Modify: root compatibility wrappers

- [x] **Step 1:** 将默认根目录从硬编码绝对路径改为脚本所在目录向上回溯得到的仓库根目录。
- [x] **Step 2:** 将 `umc_test.exp` 引用改为 `tests/integration/umc_test.exp`。
- [x] **Step 3:** 保留 `-r`/`--root-dir` 等显式覆盖参数。

### Task 3: 补充 README 本机环境与目录地图

**Files:**
- Modify: `README.md`
- Create: `tools/README.md`

- [x] **Step 1:** 添加 macOS 工具探测结果和依赖阻断说明。
- [x] **Step 2:** 添加新目录用途和推荐入口。
- [x] **Step 3:** 更新压测、诊断和集成测试命令示例。

### Task 4: 脚本级验收

**Files:**
- Verify: all moved shell/Python/Expect scripts and README references

- [x] **Step 1:** 对 Bash 脚本执行 `bash -n`，对 Python 执行 AST 语法检查（避免生成临时 pyc）。
- [x] **Step 2:** 搜索旧根路径、旧 `umc_test.exp` 路径和 `/Users/wp/` 硬编码。
- [x] **Step 3:** 复核 `git diff --check` 和工作区状态。
