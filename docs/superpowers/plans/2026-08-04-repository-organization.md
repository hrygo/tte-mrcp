# TTE-MRCP Repository Organization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 清理并重组 TTE-MRCP 仓库，使活动源码树只保留可维护源码、配置、构建规则、测试和有效文档，同时保留低价值历史文件的可恢复归档。

**Architecture:** 保留现有 `libs/`、`modules/`、`plugins/`、`platforms/`、`tests/` 分层不变；新增 `docs/reports/` 作为报告入口，新增被忽略的 `.archive/` 作为本地归档区。生成物通过 `.gitignore` 统一隔离，历史备份和运行产物只从活动树移出，不参与构建或代码图谱。

**Tech Stack:** Autotools (`configure.ac`, `Makefile.am`/`Makefile.in`), C/C++, Markdown, Git.

## Global Constraints

- 以当前源码、构建文件和配置为 SSOT；不采信历史文档对目录用途的描述。
- 保留所有有效源码、测试脚本、配置源文件及其当前修改。
- 归档优先于删除；归档区必须可通过清单恢复原路径。
- 所有 shell 命令使用 `rtk` 前缀；文件编辑使用 `apply_patch`。

### Task 1: 建立归档区和归档清单

**Files:**
- Create: `.archive/repo-cleanup-20260804/归档清单.md`
- Modify: `.gitignore`

- [ ] **Step 1: 添加归档区忽略规则和生成物模式**
- [ ] **Step 2: 编写归档清单模板，包含原路径、类别、处理动作和恢复说明**
- [ ] **Step 3: 检查规则不会忽略源码、配置源文件或测试输入**

### Task 2: 归档并移出历史备份、运行产物和构建生成物

**Files:**
- Move: `plugins/demo-recog/src/bak/` → `.archive/repo-cleanup-20260804/legacy/plugins/demo-recog/src/bak/`
- Move: `plugins/demo-synth/src/bak/` → `.archive/repo-cleanup-20260804/legacy/plugins/demo-synth/src/bak/`
- Move: generated Makefiles, `.libs`, `.deps`, objects, Libtool files, binaries, `config.*`, `libtool`, logs → matching archive categories
- Remove from active tree: tracked `.DS_Store`, tracked plugin `.so`, tracked plugin object/Libtool outputs

- [ ] **Step 1: 记录待处理路径并保存归档副本**
- [ ] **Step 2: 从活动树移出归档对象，不触碰源码和配置源文件**
- [ ] **Step 3: 更新归档清单并确认每个原路径都有归档对应项**

### Task 3: 集中有效报告

**Files:**
- Move: `TTE-MRCP技术架构分析报告.md` → `docs/reports/TTE-MRCP技术架构分析报告.md`
- Move: `呼叫中心TTS插件高并发杂音静音丢音问题排查与修复方案.md` → `docs/reports/呼叫中心TTS插件高并发杂音静音丢音问题排查与修复方案.md`

- [ ] **Step 1: 创建 `docs/reports/` 并移动两份报告**
- [ ] **Step 2: 检查 README、脚本和构建文件没有依赖报告的根路径**

### Task 4: 结构和构建验收

**Files:**
- Verify: `Makefile.am`, `configure.ac`, `plugins/*/Makefile.am`, `README.md`

- [ ] **Step 1: 检查活动树中的垃圾模式和报告入口**
- [ ] **Step 2: 运行 Autotools 配置检查，确认 Makefile 可由源文件重新生成**
- [ ] **Step 3: 复核 `git status`，确认未回退用户既有源码改动**
