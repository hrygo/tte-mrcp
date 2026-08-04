# Linux 双架构构建工作流设计

## 背景与目标

为 TTE-MRCP 新增 GitHub Actions 工作流，在每次针对 `main` 的拉取请求、推送到 `main` 以及手动触发时，分别构建可部署到以下目标的安装包：

| 目标环境 | 架构 | 包名 | ABI/工具链基线 |
| --- | --- | --- | --- |
| RHEL 7.9 | x86_64 | `tte-mrcp-rhel7-x86_64.zip` | glibc 2.17；GCC/G++ 4.8.5 |
| 银河麒麟高级服务器操作系统 V10 | aarch64 | `tte-mrcp-kylinv10-aarch64.zip` | glibc 2.28（RHEL 8 ABI 基线） |

每个 ZIP 都是可部署安装树，必须包含 `bin/`、`lib/`、`plugin/`、`conf/` 与 `data/`。`bin/` 必须包含 `unimrcpserver`、`umc`、`unimrcpclient` 与 `asrclient` 四个程序。工作流仅上传 GitHub Actions artifact，不创建 GitHub Release，也不发送外部请求。

## 已验证的可行性边界

- 当前源码的 C++ 文件未使用 `nullptr`、智能指针、`override`、`constexpr`、`noexcept` 等 C++11 语法；GCC 4.8.5 的默认 C++98 能力满足当前 UMC 源码。
- `configure.ac` 要求 APR、APR-util 和 Sofia-SIP。`build/acmacros/sofia-sip.m4` 接受 `pkg-config` 模块、安装前缀或 Sofia-SIP 构建树，因此 CI 可通过受控依赖前缀完成探测。
- Autotools 安装规则会将可执行文件装入 `bin/`，共享库装入 `lib/`，插件装入 `plugin/`，并安装 `conf/` 与 `data/`；这是 Linux 生产构建的项目优先入口。
- RHEL 7 不支持 arm64，因此 arm64 不能标记为 RHEL 7.9 兼容。麒麟 V10 ARM64 的 glibc 2.28 与 RHEL 8 ABI 基线匹配。

本工作流只能证明构建与二进制 ABI 下限；实际服务器上的配置加载、动态库加载、RTP/MRCP 建链及外部 TTS 服务互通仍须在对应部署环境验证。

## 方案比较

1. 直接在 Ubuntu runner 构建：实现最短，但 Ubuntu 的 glibc 高于 RHEL 7.9，x86_64 包无法作为 RHEL 7.9 兼容产物交付，拒绝采用。
2. 使用 QEMU 交叉构建 arm64：可减少 runner 类型，但依赖构建与运行探针不在目标 ISA 上，无法提供原生 arm64 构建证据，拒绝采用。
3. 原生 runner 加 ABI 基线容器：x86_64 使用 RHEL 7.9 兼容容器，aarch64 使用原生 ARM64 runner 和 RHEL 8 ABI 基线容器。这一方案同时满足目标 glibc 下限和原生 ISA 构建，采用。

## 工作流结构

新增 `.github/workflows/build-linux.yml`，包含一个 `build-linux` job 和两项矩阵条目。JavaScript actions（检出与 artifact 上传）始终运行在原生 Ubuntu runner；每个 job 随后通过 `docker run` 挂载工作区，在对应 ABI 基线镜像中执行编译、安装、审计和打包。这样避免 GitHub Actions 的 Node 运行时在 RHEL 7 glibc 2.17 容器中无法启动。

| matrix 字段 | x86_64 | aarch64 |
| --- | --- | --- |
| GitHub runner | `ubuntu-24.04` | `ubuntu-24.04-arm` |
| Docker 构建镜像 | RHEL 7.9 ABI 兼容镜像 | RHEL 8 ABI 基线镜像 |
| 目标 glibc 上限 | 2.17 | 2.28 |
| artifact 名称 | `tte-mrcp-rhel7-x86_64` | `tte-mrcp-kylinv10-aarch64` |
| ZIP 文件 | `tte-mrcp-rhel7-x86_64.zip` | `tte-mrcp-kylinv10-aarch64.zip` |

作业内步骤按以下顺序执行：

1. 在原生 Ubuntu runner 检出源码，并确认 Docker 可用。
2. 用 `docker run` 将工作区挂载到 ABI 基线镜像内，配置固定、可用的系统依赖来源并安装编译工具、APR、APR-util、OpenSSL、`pkg-config`；从 `freeswitch/sofia-sip` 的固定 `v1.13.17` tag 构建 Sofia-SIP，并断言其提交为 `6198851a610b7889c17e2d98fb84617bc1dd7aec`。Sofia-SIP 安装到 `/opt/tte-mrcp`，使其 RPATH 与最终部署路径一致；RHEL 7 条目不得升级 GCC/G++。
3. 输出并断言编译器版本；x86_64 必须匹配 GCC/G++ 4.8.5，aarch64 记录实际编译器版本。
4. 在 Sofia-SIP 构建完成后切回工作区；在干净构建目录执行 `./configure --prefix=/opt/tte-mrcp`、`make` 和 `make install DESTDIR="$GITHUB_WORKSPACE/package-root"`；aarch64 条目额外传入 `--build=aarch64-unknown-linux-gnu`，以绕开项目内 2009 年版 `config.guess` 对该架构的识别缺口。Sofia-SIP 通过受控安装前缀或 `pkg-config` 提供给 `configure`。
5. 检查安装树中四个可执行文件、共享库和插件；用 `file` 验证 ELF 架构，用 `readelf --version-info` 验证所有打包 ELF 文件不引用高于矩阵基线的 `GLIBC_*` 符号。
6. 将构建所用的非系统共享库依赖复制到 `$GITHUB_WORKSPACE/package-root/opt/tte-mrcp/lib/`，保留其 soname 链接；不得打包动态加载器或 glibc。随后以该 staging 目录中的 `opt/tte-mrcp/` 为部署根生成 ZIP，列出 ZIP 内容后上传 artifact。

构建前缀固定为部署路径 `/opt/tte-mrcp`，安装时通过 `DESTDIR` staging 到工作区，因此二进制 RPATH 与最终部署位置一致。ZIP 内部根目录为 `opt/tte-mrcp/`；部署时从根目录解压，或将该目录整体移动到 `/opt/tte-mrcp`。运行时依赖由包内 `lib/` 与 `plugin/` 提供。

## 失败处理与可观测性

- 依赖安装、配置、构建、安装、ABI 审计、ZIP 校验任一步失败都使对应矩阵 job 失败；`fail-fast: false` 保留另一架构的完整结果。
- RHEL 7 的归档仓库是外部可用性风险。工作流将显式配置归档源并打印启用仓库；若依赖源失效，应在日志中明确显示为环境依赖失败，而非源码兼容性结论。
- artifact 的压缩包名称与内容校验结果写入 job summary，便于审查者下载和比对。

## 验收标准

1. 工作流 YAML 可解析，且其事件、矩阵、runner、容器和 artifact 名称符合本设计。
2. 两个矩阵条目都生成对应 ZIP artifact；每个 ZIP 均包含四个指定可执行文件及 `lib/`、`plugin/`、`conf/`、`data/`。
3. x86_64 构建日志确认 GCC/G++ 4.8.5，ELF 为 x86-64，GLIBC 引用不高于 2.17。
4. aarch64 构建在原生 ARM64 runner 上完成，ELF 为 aarch64，GLIBC 引用不高于 2.28。
5. 工作流不创建 Release、不推送标签、不访问真实 MRCP/TTS 服务。

## 非目标

- 不替代 RHEL 7.9 或麒麟 V10 实机上的启动、动态库加载、MRCP/RTP、TTS 服务联调。
- 不修改 TTS 插件、公共库、CMake、Autotools、XML 配置或部署脚本。
- 不在本变更中添加 Windows、macOS 或其他 Linux 发行版的发布包。
