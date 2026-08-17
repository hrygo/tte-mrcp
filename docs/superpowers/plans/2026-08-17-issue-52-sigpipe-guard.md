# Issue #52 SIGPIPE 防护第一阶段实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 在 POSIX `unimrcpserver` 启动时忽略 SIGPIPE，使断连写入返回错误而不终止进程，并降低 TTS 敏感/逐帧 INFO 日志量。

**架构：** 在服务端应用入口新增独立、可测试的 `unimrcp_server_signals_init()` 平台适配单元；`main()` 在创建任何服务线程前调用它。TTS 继续通过 APR 发送，不绕开超时和错误映射；测试用本地 `socketpair()` 与子进程验证进程级信号行为，用源码策略测试锁定日志等级。

**技术栈：** C89/C99 兼容 C、POSIX signal/socket/fork/wait、APR/UniMRCP、Autotools、CMake、Visual Studio 工程、CTest。

---

## 文件结构

- 创建 `platforms/unimrcp-server/src/unimrcp_server_signal.h`：声明服务端进程信号初始化接口。
- 创建 `platforms/unimrcp-server/src/unimrcp_server_signal.c`：封装 POSIX SIGPIPE 忽略和 Windows no-op。
- 创建 `platforms/unimrcp-server/tests/test_unimrcp_server_signal.c`：用子进程验证默认 SIGPIPE 与防护后的 EPIPE 行为。
- 修改 `platforms/unimrcp-server/src/main.c`：在 APR 初始化后、其他服务对象创建前启用信号策略。
- 修改 `platforms/unimrcp-server/Makefile.am`：接入 helper、测试和 `make check`。
- 修改 `platforms/unimrcp-server/CMakeLists.txt`：接入 helper、测试和 CTest。
- 修改 `platforms/unimrcp-server/unimrcpserver.vcxproj`：将跨平台 helper 纳入 Win32/x64 编译。
- 修改 `platforms/unimrcp-server/unimrcpserver.vcproj`：将跨平台 helper 纳入旧 Visual Studio 工程。
- 修改 `plugins/tts-websocket/tests/test_tts_websocket_connection_log.c`：增加日志安全与高频日志等级的源码策略回归测试。
- 修改 `plugins/tts-websocket/src/tts_websocket_engine.c`：逐帧和敏感正文日志降为 DEBUG；会话汇总保持 INFO。

### 任务 1：用失败测试定义 SIGPIPE 防护行为

**文件：**
- 创建：`platforms/unimrcp-server/tests/test_unimrcp_server_signal.c`
- 创建：`platforms/unimrcp-server/src/unimrcp_server_signal.h`

- [ ] **步骤 1：创建仅含接口声明的头文件**

```c
#ifndef UNIMRCP_SERVER_SIGNAL_H
#define UNIMRCP_SERVER_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

int unimrcp_server_signals_init(void);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **步骤 2：编写 POSIX 进程行为测试**

测试文件在 Windows 打印 skip 并返回 0；POSIX 分支包含 `errno.h`、`signal.h`、`sys/socket.h`、`sys/wait.h` 和 `unistd.h`，实现以下两个测试：

```c
static int child_write_result(int initialize_signals)
{
    int sockets[2];
    pid_t pid;
    int status;

    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return 0;
    pid = fork();
    if(pid == 0) {
        char byte = 'x';
        close(sockets[0]);
        signal(SIGPIPE, SIG_DFL);
        if(initialize_signals && unimrcp_server_signals_init() != 0) _exit(10);
        if(write(sockets[1], &byte, 1) == -1 && errno == EPIPE) _exit(0);
        _exit(11);
    }
    close(sockets[0]);
    close(sockets[1]);
    if(pid < 0 || waitpid(pid, &status, 0) != pid) return 0;
    if(initialize_signals) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE;
}
```

`main()` 依次断言 `child_write_result(0)` 和 `child_write_result(1)`，失败打印明确消息，成功打印 `2/2 signal tests passed`。

- [ ] **步骤 3：直接编译测试并验证红灯**

运行：

```bash
cc -Iplatforms/unimrcp-server/src \
  platforms/unimrcp-server/tests/test_unimrcp_server_signal.c \
  -o /tmp/test_unimrcp_server_signal
```

预期：链接失败，报 `unimrcp_server_signals_init` 未定义；这证明测试在等待生产实现，而不是测试已有行为。

- [ ] **步骤 4：提交红灯测试**

```bash
git add platforms/unimrcp-server/src/unimrcp_server_signal.h \
  platforms/unimrcp-server/tests/test_unimrcp_server_signal.c
git commit -m "test(server): reproduce SIGPIPE termination" -m "Refs #52"
```

### 任务 2：实现服务端进程级 SIGPIPE 防护

**文件：**
- 创建：`platforms/unimrcp-server/src/unimrcp_server_signal.c`
- 修改：`platforms/unimrcp-server/src/main.c:18-190`
- 修改：`platforms/unimrcp-server/Makefile.am:1-8`
- 修改：`platforms/unimrcp-server/CMakeLists.txt:7-25`
- 修改：`platforms/unimrcp-server/unimrcpserver.vcxproj:120-135`
- 修改：`platforms/unimrcp-server/unimrcpserver.vcproj` 的 `Files` 源文件组

- [ ] **步骤 1：写最小跨平台实现**

```c
#include "unimrcp_server_signal.h"

#ifdef WIN32
int unimrcp_server_signals_init(void)
{
    return 0;
}
#else
#include <signal.h>

int unimrcp_server_signals_init(void)
{
    return signal(SIGPIPE, SIG_IGN) == SIG_ERR ? -1 : 0;
}
#endif
```

- [ ] **步骤 2：运行单测试验证绿灯**

运行：

```bash
cc -Iplatforms/unimrcp-server/src \
  platforms/unimrcp-server/tests/test_unimrcp_server_signal.c \
  platforms/unimrcp-server/src/unimrcp_server_signal.c \
  -o /tmp/test_unimrcp_server_signal && /tmp/test_unimrcp_server_signal
```

预期：输出 `2/2 signal tests passed`，退出码 0。

- [ ] **步骤 3：在服务端入口尽早初始化**

在 `main.c` 包含头文件，并紧接 APR 成功初始化后调用：

```c
#include "unimrcp_server_signal.h"

/* APR global initialization */
if(apr_initialize() != APR_SUCCESS) {
    apr_terminate();
    return 1;
}
if(unimrcp_server_signals_init() != 0) {
    fprintf(stderr, "Failed to initialize server signal handling\n");
    apr_terminate();
    return 1;
}
```

信号初始化必须发生在 pool、logger、daemon/service 和插件线程创建之前。

- [ ] **步骤 4：接入 Autotools 和 CMake**

`Makefile.am` 将 `src/unimrcp_server_signal.c` 和头文件加入 `unimrcpserver_SOURCES`，并添加：

```make
check_PROGRAMS = test_unimrcp_server_signal
TESTS = $(check_PROGRAMS)
test_unimrcp_server_signal_SOURCES = \
    tests/test_unimrcp_server_signal.c \
    src/unimrcp_server_signal.c
test_unimrcp_server_signal_CPPFLAGS = -I$(top_srcdir)/platforms/unimrcp-server/src
```

`CMakeLists.txt` 将 helper 加入 `UNIMRCP_SERVER_SOURCES`，定义默认开启的 `BUILD_UNIMRCP_SERVER_TESTS` 选项，并在 `BUILD_UNIMRCP_SERVER_TESTS AND NOT WIN32` 时创建 `test_unimrcp_server_signal`、添加 src include directory 和 CTest 条目。Windows 由应用目标编译 no-op 分支。

- [ ] **步骤 5：接入 Visual Studio 工程**

`unimrcpserver.vcxproj` 的 `ClCompile` 组加入：

```xml
<ClCompile Include="src\unimrcp_server_signal.c" />
```

并添加头文件项：

```xml
<ClInclude Include="src\unimrcp_server_signal.h" />
```

旧 `unimrcpserver.vcproj` 在 Header Files 与 Source Files 分组分别加入等价 `File RelativePath`，不创建 POSIX 测试目标。

- [ ] **步骤 6：运行服务端测试和配置检查**

运行：

```bash
cmake -S . -B /tmp/tte-mrcp-issue52-cmake \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/tte-mrcp-issue52-cmake \
  --target test_unimrcp_server_signal
ctest --test-dir /tmp/tte-mrcp-issue52-cmake \
  -R unimrcp_server_signal --output-on-failure
```

预期：configure 和目标构建成功，CTest 1/1 PASS。若根 CMake 的既有依赖阻断发生，记录准确阻断，并至少保留步骤 2 的独立行为测试结果。

- [ ] **步骤 7：提交生产实现与构建接入**

```bash
git add platforms/unimrcp-server
git commit -m "fix(server): ignore SIGPIPE during startup" -m "Refs #52"
```

### 任务 3：用失败测试锁定 TTS 日志策略

**文件：**
- 修改：`plugins/tts-websocket/tests/test_tts_websocket_connection_log.c`

- [ ] **步骤 1：增加源码日志策略断言**

在现有 source 读取测试中加入辅助函数，要求以下高频或敏感日志使用 DEBUG：

```c
static int log_statement_uses_debug(const char *source, const char *message)
{
    const char *hit = strstr(source, message);
    const char *line;
    if(!hit) return 0;
    line = hit;
    while(line > source && line[-1] != '\n') --line;
    return strstr(line, "APT_PRIO_DEBUG") &&
           strstr(line, "APT_PRIO_DEBUG") < hit;
}
```

对下列消息逐一断言：

- `[WS] websocket_recv_message returned`
- `[TIMING] Audio frame #`
- `[WS] Sentence text:`
- `[WS] Sent session.config:`
- `[WS] Request body:`
- `[WS] Original text:`

保持 `[TIMING] ========== Session Summary ==========` 为 INFO 的反向断言，防止把会话汇总一并降级。

- [ ] **步骤 2：编译运行并验证红灯**

运行：

```bash
cc $(pkg-config --cflags apr-1) \
  plugins/tts-websocket/tests/test_tts_websocket_connection_log.c \
  $(pkg-config --libs apr-1) \
  -o /tmp/test_tts_websocket_connection_log && \
  /tmp/test_tts_websocket_connection_log \
  plugins/tts-websocket/src/tts_websocket_engine.c
```

预期：FAIL，首先报告 `websocket_recv_message returned` 或敏感正文仍使用 INFO。

- [ ] **步骤 3：提交红灯测试**

```bash
git add plugins/tts-websocket/tests/test_tts_websocket_connection_log.c
git commit -m "test(tts): define production log policy" -m "Refs #52"
```

### 任务 4：降低逐帧和敏感正文日志等级

**文件：**
- 修改：`plugins/tts-websocket/src/tts_websocket_engine.c:654-877`
- 修改：`plugins/tts-websocket/src/tts_websocket_engine.c:1359-1368`

- [ ] **步骤 1：实施最小日志等级修改**

仅将任务 3 列出的六类日志从 `APT_PRIO_INFO` 改为 `APT_PRIO_DEBUG`。保留以下内容为 INFO：

```c
LOG_WITH_SID(synth_channel, APT_PRIO_INFO,
    "[TIMING] ========== Session Summary ==========");
LOG_WITH_SID(synth_channel, APT_PRIO_INFO,
    "[TIMING] First audio frame arrived: ...");
```

不得改变 WebSocket payload、状态机、buffer 或完成事件逻辑。

- [ ] **步骤 2：运行日志策略测试验证绿灯**

运行任务 3 步骤 2 的同一命令。

预期：所有 connection/log policy 测试通过，退出码 0。

- [ ] **步骤 3：运行 TTS 单元回归**

运行：

```bash
cmake --build /tmp/tte-mrcp-issue52-cmake --target \
  test_tts_websocket_http_parse \
  test_tts_websocket_pcm \
  test_tts_websocket_lifecycle \
  test_tts_websocket_ws \
  test_tts_websocket_thread \
  test_tts_websocket_connection_log
ctest --test-dir /tmp/tte-mrcp-issue52-cmake \
  -R 'tts_websocket_(http_parse|pcm|lifecycle|ws|thread|connection_log)' \
  --output-on-failure
```

预期：所有已构建的 TTS 测试 PASS；HTTP parser 保持 13/13 基线。

- [ ] **步骤 4：提交日志修改**

```bash
git add plugins/tts-websocket/src/tts_websocket_engine.c
git commit -m "fix(tts): reduce per-frame production logging" -m "Refs #52"
```

### 任务 5：安全、自检和交付门禁

**文件：**
- 修改：`docs/superpowers/plans/2026-08-17-issue-52-sigpipe-guard.md`（勾选实际完成步骤）
- 不创建生成物或运行日志到仓库

- [ ] **步骤 1：执行安全检查**

确认：没有自定义 signal handler；没有在信号上下文记录日志；断连错误不包含文本正文、凭据或音频数据；不存在无限发送重试；测试只使用 `socketpair()`。

运行：

```bash
git diff origin/main --check
git diff origin/main -- plugins/tts-websocket/src/tts_websocket_engine.c \
  platforms/unimrcp-server
```

预期：`diff --check` 无输出；审阅内容仅包含 Issue #52 范围。

- [ ] **步骤 2：执行构建系统门禁**

运行：

```bash
./bootstrap
./configure --help
cmake -S . -B /tmp/tte-mrcp-issue52-final \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

预期：命令成功；若遇到 AGENTS.md 已记录的 `JB_TRACE/RTP_TRACE` 基线阻断，必须将其归因到既有 MPF 问题，不归因于本变更。

- [ ] **步骤 3：检查跨平台工程引用**

运行：

```bash
rg -n 'unimrcp_server_signal' \
  platforms/unimrcp-server/Makefile.am \
  platforms/unimrcp-server/CMakeLists.txt \
  platforms/unimrcp-server/unimrcpserver.vcxproj \
  platforms/unimrcp-server/unimrcpserver.vcproj
```

预期：Autotools、CMake、VS2010+ 与旧 VS 工程均包含 helper；Windows 分支不引用 SIGPIPE。

- [ ] **步骤 4：重新索引代码图谱并验证节点**

调用 codebase-memory-mcp `index_repository`，仓库路径为当前工作区、模式 `moderate`；随后搜索 `unimrcp_server_signals_init` 和 `websocket_send_all`，并追踪 `unimrcp_server_signals_init` 的调用者。

预期：图谱包含 helper 定义及 `main` 调用边，TTS 发送调用链保持不变。

- [ ] **步骤 5：提交计划完成状态**

```bash
git add docs/superpowers/plans/2026-08-17-issue-52-sigpipe-guard.md
git commit -m "docs: record issue 52 verification" -m "Refs #52"
```

- [ ] **步骤 6：最终审查**

运行：

```bash
git status --short
git log --oneline origin/main..HEAD
git diff --stat origin/main...HEAD
```

预期：工作区干净；提交均关联 #52；变更不包含共享 consumer 或 MRCP session 状态机修改。Windows 和 Linux 若未在真实平台执行，交付说明明确标记为“未验证”。
