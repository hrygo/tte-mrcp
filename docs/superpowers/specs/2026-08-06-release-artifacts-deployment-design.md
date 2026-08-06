# Release Artifacts 与服务器部署设计

## 目标

当 GitHub 推送 `v*` tag 或 `release/**` 分支时，自动执行现有 Linux 双架构构建、运行验证和 WebSocket E2E 测试，并产出可下载的部署 ZIP Artifacts。`v*` tag 额外创建 GitHub Release 并附加 ZIP 与 SHA256 校验文件；`release/**` 只保留 Actions Artifacts，供候选版本验证。

## 范围与不变边界

- 复用 `.github/workflows/build-linux.yml` 的 RHEL 7 x86_64 和 Kylin V10 AArch64 构建矩阵。
- 保留现有 PR、`main` push 和手动触发，以及既有编译、ABI、动态库、插件单测、运行时和 E2E 验证。
- 不在 GitHub Actions 中自动连接真实生产 TTS 服务、修改服务器配置或执行远程发布。
- 不修改用户当前已存在的工作区改动。

## 触发与版本语义

Workflow 继续支持：

- `pull_request` 到 `main`：验证代码，不发布部署资产。
- `push` 到 `main`：验证主线，不发布部署资产。
- `push` 到 `v*` tag：验证并发布正式 Release。
- `push` 到 `release/**`：验证并上传候选版本 Actions Artifacts。
- `workflow_dispatch`：手动验证，不自动创建 Release。

发布构建使用当前 Git ref 和 commit SHA 生成版本元数据，避免同名候选包混淆。正式 Release 使用 tag 作为版本号；候选分支使用安全化分支名和短 SHA 作为 Artifact 标识。

## Artifact 内容

每个架构产出一个 ZIP：

- `tte-mrcp-rhel7-x86_64.zip`
- `tte-mrcp-kylinv10-aarch64.zip`

ZIP 内保持现有安装前缀 `/opt/tte-mrcp`，包含 `bin/`、`lib/`、`plugin/`、`conf/`、`data/` 及运行所需的非系统动态库。每个 ZIP 旁边生成 SHA256 校验文件和包含 ref、SHA、架构、构建基线及验证结果的版本元数据。

GitHub Release 只在事件为 `push` 且 ref 为 `refs/tags/v*` 时创建，并上传所有架构 ZIP、SHA256 文件和版本元数据。Release 生成失败不能绕过前置构建和验证；候选分支不创建 Release。

## 服务器部署与绿灯测试

文档新增从 GitHub Actions 或 Release 下载对应架构 ZIP 的步骤，要求先校验 SHA256，再解压到版本目录并通过软链接切换当前版本。部署说明覆盖目标架构、系统 ABI、目录布局、动态库加载、配置、启停、日志、端口和插件检查，以及 loopback fixture、UMC/压力和 WebSocket E2E 测试。真实 TTS 服务调用作为人工联调项。

绿灯判定同时检查服务进程存活、插件成功加载、MRCP profile 就绪、请求完成、有效音频/响应结果和异常关闭为零；`SPEAK-COMPLETE` 单独不能证明音频完整。

## 验证

- YAML 语法和 workflow 结构静态检查。
- `git diff --check`。
- workflow 中的发布条件、资产列表、校验文件和非 tag 行为通过脚本化检查。
- 部署文档中的命令与当前 `tools/diagnostics`、`tools/stress` 入口逐项核对。
- 受环境限制时，明确区分本地静态验证与 GitHub runner/真实服务器验证。
