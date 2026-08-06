# Release Artifacts 部署与绿灯测试

本文说明如何使用 GitHub Actions 产出的构建包在服务器部署 TTE-MRCP，并完成发布后的基础验收。发布包应来自 GitHub Release 或发布分支对应的 Actions run；不要在生产机直接从源码临时构建来替代发布包。

## 1. 获取发布包

GitHub 支持两种触发方式：

1. 创建版本 tag 并发布 GitHub Release。打开仓库的 **Releases** 页面，进入目标版本，在 **Assets** 下载与服务器架构匹配的压缩包及对应的 `.sha256` 校验文件。
2. 推送发布分支。打开该分支触发的 GitHub Actions run，在 **Artifacts** 区域下载构建包及校验文件。只有 workflow 显示成功（绿色）时才允许部署；Artifact 的保留期以仓库 Actions 设置为准。

当前 CI 产物文件名固定为：

- RHEL 7 x86_64：`tte-mrcp-rhel7-x86_64.zip`
- Kylin V10 AArch64：`tte-mrcp-kylinv10-aarch64.zip`

下载后在临时目录使用与 ZIP 同名的 checksum 文件核对，例如：

```sh
cd /tmp/tte-mrcp-release
sha256sum -c tte-mrcp-rhel7-x86_64.zip.sha256
```

输出必须包含 `OK`，且命令退出码为 0。Kylin V10 AArch64 使用 `tte-mrcp-kylinv10-aarch64.zip` 及其对应 checksum 文件。不能把 RHEL 7 x86_64 包部署到 AArch64，也不能用其他架构的 checksum 文件校验。

## 2. 安装目录与回滚

以 root 或具有 `/opt/tte-mrcp` 写权限的发布账号执行。每个版本独立解压，`current` 只在完整校验后切换：

```sh
VERSION=<version>
ARCHIVE=/tmp/tte-mrcp-rhel7-x86_64.zip
SHA256=${ARCHIVE}.sha256

cd "$(dirname "$ARCHIVE")"
sha256sum -c "$SHA256"
install -d -m 0755 "/opt/tte-mrcp/releases/$VERSION"
unzip -q "$ARCHIVE" -d "/opt/tte-mrcp/releases/$VERSION"
test -x "/opt/tte-mrcp/releases/$VERSION/opt/tte-mrcp/bin/unimrcpserver"
ln -sfn "/opt/tte-mrcp/releases/$VERSION/opt/tte-mrcp" /opt/tte-mrcp/current
```

Kylin V10 AArch64 使用同样步骤，将 `ARCHIVE` 替换为 `/tmp/tte-mrcp-kylinv10-aarch64.zip`，并使用其对应的 `.sha256` 文件。ZIP 内只有 `opt/tte-mrcp/{bin,lib,plugin,conf,data}`，不包含 `tools/diagnostics`、`tools/stress` 或源码。确认 `readlink -f /opt/tte-mrcp/current` 指向 `/opt/tte-mrcp/releases/<version>/opt/tte-mrcp` 后再启动服务。回滚时停止服务，将 `current` 指向上一个已验收目录，再按本文的启动和绿灯检查重新执行；不要覆盖已有版本目录。

## 3. 启动前配置

`/opt/tte-mrcp/current` 应包含 `bin/unimrcpserver`、`bin/umc`、`lib/`、`plugin/`、`conf/` 和 `data/`。将外部 TTS WebSocket endpoint 写入当前版本的服务配置，核对 host、port、path、认证信息和 TLS/明文协议与供应商约定一致；敏感值不要写入命令行或提交到仓库。ASR 如启用 FunASR，核对 `funasr-host`、`funasr-port` 和 `funasr-path`。

服务运行时从发布包加载共享库：

```sh
export RELEASE_ROOT=/opt/tte-mrcp/current
export LD_LIBRARY_PATH="$RELEASE_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$RELEASE_ROOT"
```

启动前检查插件、配置和端口：

```sh
test -x "$RELEASE_ROOT/bin/unimrcpserver"
test -x "$RELEASE_ROOT/bin/umc"
test -f "$RELEASE_ROOT/plugin/tts_websocket.so"
test -f "$RELEASE_ROOT/plugin/asr_websocket.so"
test -f "$RELEASE_ROOT/conf/unimrcpserver.xml"
ss -lntp | grep -E ':(8060|1544)\b' || true
```

默认 SIP 端口为 `8060`，默认 RTSP 端口为 `1544`；如配置已改动，以实际 XML 和防火墙规则为准。检查日志必须能看到 MRCPv2 profile 创建/服务启动，并且不能出现 `Failed to Load UniMRCP Server Document`、`Failed to compose plugin path`、`Failed to Create Listening Socket`、`Failed to Create NUA` 或 `Failed to Run Sofia-SIP Task`。确认插件日志使用 canonical 名称 `tts_websocket`、`asr_websocket`，不要把旧的 `demosynth`/`demorecog` 名称当作通过标准。

## 4. 启停服务

建议由 systemd 或现有运维脚本托管，启动命令保持发布根目录固定：

```sh
cd /opt/tte-mrcp/current
export LD_LIBRARY_PATH="/opt/tte-mrcp/current/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./bin/unimrcpserver -r /opt/tte-mrcp/current -l 6 -o 1
```

在前台确认日志无配置、插件和端口错误后，再交给服务管理器后台运行。停止时先发送 `TERM`，等待进程退出并确认端口释放；只有确认进程未退出时才使用 `KILL`：

```sh
kill -TERM "$(pgrep -n -f '/opt/tte-mrcp/current/bin/unimrcpserver')"
sleep 2
pgrep -f '/opt/tte-mrcp/current/bin/unimrcpserver' || true
ss -lntp | grep -E ':(8060|1544)\b' || true
```

## 5. 绿灯测试

发布 ZIP 不包含测试工具。绿灯测试应在 CI runner 或部署辅助机执行：该机器必须 checkout 与发布 tag/发布分支构建对应的源码 commit，并从该 checkout 提供 `tools/diagnostics`、`tools/stress` 和 `tests/integration`。测试工具目录不能从其他 commit 复制。测试机还应安装 `expect`、`python3`、`timeout`、`stdbuf`、`ss` 和 `unzip`，并确认到外部 TTS/ASR endpoint 的网络策略已放行。下面以源码 checkout 为 `$CHECKOUT`、解压后的发布包运行目录为 `$PACKAGE_DIR`。

### 5.1 FunASR fixture self-test

先验证 fixture 本身，不依赖外部 FunASR 服务：

```sh
CHECKOUT=/srv/src/tte-mrcp-<version>
PACKAGE_DIR=/opt/tte-mrcp/current
python3 "$CHECKOUT/tools/diagnostics/funasr_ws_fixture.py" --self-test
```

成功标准：命令退出码为 0，输出没有 traceback 或 failure。该检查只证明本地 fixture 可用，不证明真实服务可用。

### 5.2 TTS/ASR loopback E2E

该命令使用源码 checkout 中的 fixture 和 Expect 测试工具，启动本地 TTS/ASR WebSocket fixture，并启动发布包中的 `unimrcpserver` 和 `umc`，覆盖插件加载、配置、MRCP 建链、TTS RTP 与 ASR NLSML：

```sh
CHECKOUT=/srv/src/tte-mrcp-<version>
PACKAGE_DIR=/opt/tte-mrcp/current
ROOT_DIR="$CHECKOUT" \
PACKAGE_DIR="$PACKAGE_DIR" \
E2E_MODE=split \
"$CHECKOUT/tools/diagnostics/run_websocket_e2e.sh"
```

成功标准：退出码为 0，并输出 `websocket E2E passed: TTS RTP + ASR NLSML against loopback mocks`；日志包含 `RESULT:SUCCESS`，TTS 有 RTP 媒体统计，ASR NLSML 包含 `fixture-asr`，且无插件加载、端口绑定或配置错误。该测试使用 loopback mock，不替代真实 TTS 联调。

### 5.3 压测

`stress_test_improved.sh` 当前按源码树布局查找 `platforms/umc/umc`、`conf/`、`tests/integration/umc_test.exp` 和 `data/`，不能直接把 ZIP 的 `/opt/tte-mrcp/current` 作为 `-r`。因此该脚本应在与发布 commit 对应、已构建完成的源码 checkout 上执行；如果只部署 ZIP，则由 CI runner 或部署辅助机执行压测，并通过服务端口验证目标服务器。先做单轮低并发烟测，再按业务容量执行多轮压力测试：

```sh
CHECKOUT=/srv/src/tte-mrcp-<version>
PACKAGE_DIR=/opt/tte-mrcp/current
"$CHECKOUT/tools/stress/stress_test_improved.sh" -r "$CHECKOUT" -t tts -c 1 -i 1 --tts-save
"$CHECKOUT/tools/stress/stress_test_improved.sh" -r "$CHECKOUT" -t tts -c 10 -i 10 -d 2 --tts-save
```

这里的 `PACKAGE_DIR` 是正在运行并被压测的解压发布包；当前 stress 脚本没有 `PACKAGE_DIR` 选项，`-r` 必须保持为对应 commit 的源码 checkout，以取得 `platforms/umc`、`conf`、`tests/integration` 和源码中的测试数据。ASR 使用仓库 `data/` 中的 PCM fixture；如服务器有专用音频目录，用 `-a` 指定：

```sh
PACKAGE_DIR=/opt/tte-mrcp/current
"$CHECKOUT/tools/stress/stress_test_improved.sh" -r "$CHECKOUT" \
  -t asr -c 1 -i 1 -a "$PACKAGE_DIR/data" -o /tmp/tte-asr-results.csv
"$CHECKOUT/tools/stress/stress_test_improved.sh" -r "$CHECKOUT" \
  -t all -c 10 -i 5 -d 2 -a "$PACKAGE_DIR/data" \
  -o /tmp/tte-asr-results.csv --tts-save \
  --server-log=/tmp/tte-transport.log \
  --fixture-report=/tmp/tte-fixture.jsonl \
  --pacing-json=/tmp/tte-pacing.json
```

成功标准：脚本退出码为 0；每个请求均有 `RESULT:SUCCESS`/对应完成事件，无 `SIP_ERROR`、`MRCP_ERROR`、`TIMEOUT`、`NODATA` 或异常关闭；TTS 产生非空录音/RTP，ASR CSV 有预期行数和识别结果。检查服务日志中的首包延迟、帧间隔、读写字节、丢弃数、静音帧和完成原因；任何 `Address already in use`、`Failed to Run Sofia-SIP` 或插件 transport error 都视为不通过。

## 6. 真实 TTS 服务联调

loopback fixture 和压力测试可以证明发布包、插件、MRCP/RTP 链路和本地测试替身工作正常，但不能证明真实 TTS 服务的认证、限流、网络路径、音质、采样率或服务端关闭语义。切换到真实 endpoint、执行生产流量或宣布发布绿灯前，必须由部署方人工确认 endpoint 配置、凭据、网络和监控，并至少完成一次真实 TTS `SPEAK`、RTP 播放/录音检查及失败重试/停止场景；相关结果应记录在发布验收单中。
