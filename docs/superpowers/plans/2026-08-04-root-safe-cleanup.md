# TTE-MRCP Root Safe Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将根目录明确的备份文件和本机生成目录移入现有本地归档区，同时保留所有有效构建输入、源码和测试入口。

**Architecture:** 采用可恢复移动而不是删除：`configure~` 归入 `legacy/root/`，`build/local/` 归入 `generated/build/local/`。根目录 `build/` 的 Autotools 源码输入和其他工程入口保持原位，归档目录继续由 `.gitignore` 隔离。

**Tech Stack:** Bash, Python 3, Autotools, Markdown, `rtk` shell wrapper.

## Global Constraints

- 当前 C/C++ 源码、构建文件和 `conf/` 配置是事实来源。
- 不移动 `build/` 根目录的 Autotools 源码输入。
- 保留仍在使用的根目录工程入口；已迁移到 `tools/` 的重复压测副本不属于保留入口。
- 归档优先于删除，保留原相对路径和恢复说明。
- 所有 shell 命令使用 `rtk` 前缀；文件编辑使用 `apply_patch`。
- 当前 `.git` 已按用户明确要求删除，不执行提交、分支或 Git 状态操作。

---

### Task 1: 预检归档目标和活动树边界

**Files:**
- Verify: `configure~`
- Verify: `build/local/`
- Verify: `.archive/repo-cleanup-20260804/`

- [x] **Step 1: 确认待归档对象存在**

Run:

```sh
rtk ls -l configure~
rtk ls -ld build/local
```

Expected: 两个路径均存在；如果任一路径不存在，停止并报告实际状态，不创建替代文件。

- [x] **Step 2: 确认归档目标没有同名冲突**

Run:

```sh
rtk ls -l .archive/repo-cleanup-20260804/legacy/root/configure~
rtk ls -ld .archive/repo-cleanup-20260804/generated/build/local
```

Expected: 目标不存在；若已存在，停止并先比较内容，避免覆盖既有归档。

### Task 2: 移动明确备份和本机生成目录

**Files:**
- Move: `configure~` → `.archive/repo-cleanup-20260804/legacy/root/configure~`
- Move: `build/local/` → `.archive/repo-cleanup-20260804/generated/build/local/`

- [x] **Step 1: 创建精确归档父目录**

Run:

```sh
rtk mkdir -p .archive/repo-cleanup-20260804/legacy/root .archive/repo-cleanup-20260804/generated/build
```

- [x] **Step 2: 移动备份文件**

Run:

```sh
rtk mv configure~ .archive/repo-cleanup-20260804/legacy/root/configure~
```

- [x] **Step 3: 移动本机生成目录**

Run:

```sh
rtk mv build/local .archive/repo-cleanup-20260804/generated/build/local
```

Expected: 文件内容和执行权限随移动保留；`build/` 根目录的源码输入不受影响。

### Task 3: 固化忽略规则和归档记录

**Files:**
- Modify: `.gitignore`
- Modify: `.archive/repo-cleanup-20260804/归档清单.md`

- [x] **Step 1: 添加备份文件忽略规则**

在 `.gitignore` 的生成物/备份规则区域加入：

```gitignore
configure~
```

不要把 `Makefile.in`、`configure`、`configure.ac` 或 `build/` 加入忽略规则。

- [x] **Step 2: 追加本次归档记录**

记录以下两项：

```markdown
## 2026-08-04 根目录安全整理增量

| 原路径 | 归档路径 | 类别 | 恢复方式 |
|---|---|---|---|
| `configure~` | `legacy/root/configure~` | 备份文件 | 复制回根目录 |
| `build/local/` | `generated/build/local/` | 本机配置/构建生成物 | 复制回 `build/local/` |
```

### Task 4: 活动树和脚本验收

**Files:**
- Verify: `build/`, `tools/`, `shell/`, `tests/`, `README.md`, `AGENTS.md`

- [x] **Step 1: 检查归档结果和保留边界**

Run:

```sh
rtk ls -l configure~
rtk ls -ld build/local
rtk ls -l .archive/repo-cleanup-20260804/legacy/root/configure~
rtk ls -ld .archive/repo-cleanup-20260804/generated/build/local
rtk ls -l build/Makefile.am build/Makefile.in build/acmacros build/rules build/pkgconfig
```

Expected: 前两个活动路径不存在，后两个归档路径存在，`build/` 源码输入仍存在。

- [x] **Step 2: 扫描旧路径和无意引用**

Run:

```sh
rtk rg -n -I -g '!.archive/**' 'configure~|build/local' README.md AGENTS.md Makefile.am CMakeLists.txt configure.ac tools shell tests docs
```

Expected: 只保留文档中描述本地构建目录的有效说明，不出现指向已归档文件的执行依赖。

- [x] **Step 3: 运行脚本静态检查**

Run:

```sh
rtk bash -n diagnose.sh stress_test.sh stress_test_simple.sh shell/*.sh tools/diagnostics/*.sh tools/stress/*.sh
rtk python3 - <<'PY'
import ast
from pathlib import Path
for path in [Path('stress_test.py'), *Path('tools/stress').glob('*.py')]:
    ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
PY
```

Expected: all commands exit 0.

- [x] **Step 4: 确认归档清单覆盖两项移动**

Run:

```sh
rtk rg -n 'configure~|build/local|根目录安全整理增量' '.archive/repo-cleanup-20260804/归档清单.md'
```

Expected: 两个原路径和本次增量标题均有记录。
