# Issue #56 ASR rapid STOP 竞态修复实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 当 ASR worker 在 STOP drain 前关闭时，可靠返回 STOP 和 channel close 响应，避免会话超时及释放竞态。

**架构：** `funasr_control` 在引擎 task 上串行处理 transport 事件。保留 `WORKER_CLOSED` 作为唯一释放栅栏，在该栅栏中先 join worker，再结算待处理 STOP，最后放行 channel close。失败 fence 通过最多 3 次立即 consumer-task 重投；仅已 join 的 worker 可在耗尽时 fallback，且 fallback 不保证仍可送达 STOP response。

**技术栈：** C、APR、UniMRCP recognizer engine、现有 `test_funasr_control` 单测。

---

## 文件结构

- 修改：`plugins/asr-websocket/tests/test_funasr_control.c` — 新增 STOP 后 worker close 的状态机回归测试。
- 修改：`plugins/asr-websocket/src/funasr_control.c` — 在 close fence 上结算待处理 STOP。
- 修改：`docs/superpowers/specs/2026-08-18-issue-56-asr-rapid-stop-race-design.md` — 记录最终验证结果。

### 任务 1：证明并修复 close-fence STOP 丢失

**文件：**

- 修改：`plugins/asr-websocket/tests/test_funasr_control.c`
- 修改：`plugins/asr-websocket/src/funasr_control.c`

- [x] **步骤 1：编写失败的测试**

在 `test_funasr_control.c` 新增 `test_worker_close_settles_pending_stop`：初始化 generation，调用 `funasr_control_request_stop`，构造同 generation 的 `FUNASR_EVENT_WORKER_CLOSED` 并处理；断言 `joins == 1`、`stop_responses == 1`、`close_responses == 1`，并将测试加入 `main`。

- [x] **步骤 2：运行测试验证失败**

运行：`make -C plugins/asr-websocket test_funasr_control && plugins/asr-websocket/test_funasr_control`

预期：测试失败，`stop_responses == 0`，证明 close fence 目前丢弃待处理 STOP。

- [x] **步骤 3：编写最少实现代码**

在 `funasr_control_handle_event` 的 `FUNASR_EVENT_WORKER_CLOSED` 分支中，在清除 `stop_pending` 前保存其值。join 成功后，若保存值为真，调用 `send_stop_response`；只有该调用成功后才调用 `send_close_response`。任何发送失败均返回 `FALSE`，不释放 close fence。

- [x] **步骤 4：运行测试验证通过**

运行：`make -C plugins/asr-websocket test_funasr_control && plugins/asr-websocket/test_funasr_control`

预期：`PASS test_funasr_control`，新增测试和现有状态机测试均通过。

- [x] **步骤 5：Commit**

提交范围：`plugins/asr-websocket/src/funasr_control.c`、`plugins/asr-websocket/tests/test_funasr_control.c` 与设计文档；提交信息：`fix(asr): settle pending STOP on worker close`。

### 任务 2：完成变更验证

**文件：**

- 修改：`docs/superpowers/specs/2026-08-18-issue-56-asr-rapid-stop-race-design.md`

- [x] **步骤 1：运行插件验证**

运行：`make -C plugins/asr-websocket check`。

预期：ASR WebSocket 插件的已构建单测全部通过；若当前树未配置，记录实际缺失的构建前置条件。

- [x] **步骤 2：运行变更卫生检查**

运行：`git diff --check && find tools -type f -name "*.sh" -print0 | xargs -0 bash -n`。

预期：两项均以 0 退出。

- [x] **步骤 3：记录结果**

将执行的命令、结果和无法在本机执行的 RHEL 7 / Kylin rapid-stop 运行时门禁写入设计文档的“验证”章节；不把本机单测表述为双架构 CI 验证。

## 自检

- Issue 的 STOP 终止、worker join、close 顺序和“不崩溃”目标均由任务 1 覆盖。
- 计划不包含占位符，且仅修改 ASR 插件与证据文档。

## 追加完成项（审查闭环）

- [x] close response 失败保留 registry、transport 和 `close_response_pending`；成功 STOP 用 `stop_responded` 防止 fence 重投重复发送。
- [x] retry lifecycle 使用有界 3 次立即 task 重投，覆盖 allocator/requeue 失败、joined-only fallback 和 unjoined retain。
- [x] retry lifecycle 测试使用 APR mutex/condition 同步等待实际 task callback。
- [x] CMake、Autotools `Makefile.am`/生成的 `Makefile.in` 与 Visual Studio `.vcproj`/`.vcxproj`/filters 均登记 retry module。
- [x] 记录本机 CMake/CTest/Autotools 实际结果；RHEL 7、Kylin、Windows MSBuild 与外部 FunASR runtime 明确标为未验证。
