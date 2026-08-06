# Release Artifacts 与服务器部署实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 为 `v*` tag 和 `release/**` 分支提供自动构建、验证、Artifacts 及正式 Release 资产，并记录服务器部署和绿灯测试方法。

**架构：** 在现有 Linux 双架构矩阵上增加发布事件判断、版本元数据、SHA256 校验和 tag-only Release job。构建与运行验证仍由现有 job 完成，部署文档复用仓库已有诊断 fixture、压力测试和 E2E 入口。

**技术栈：** GitHub Actions YAML、Bash、ZIP/SHA256、Autotools、Docker、Markdown。

---

## 文件清单

- 修改：`.github/workflows/build-linux.yml`，增加发布触发、版本命名、校验文件、元数据和 tag-only Release。
- 创建：`docs/deployment/release-artifacts.md`，记录下载、校验、部署、启动和绿灯测试。
- 修改：`README.md`，增加发布产物和部署文档入口。
- 创建：`tools/ci/check-release-workflow.sh`，对 workflow 关键发布约束做静态检查。

### 任务 1：扩展 workflow 触发与发布上下文

**文件：**
- 修改：`.github/workflows/build-linux.yml`

- [ ] **步骤 1：添加触发分支和 tag 规则**

在现有 `on` 下增加 `release/**` 分支和 `v*` tag 规则，同时保留 `pull_request`、`main` push 和 `workflow_dispatch`。

- [ ] **步骤 2：定义发布条件表达式**

使用 `github.ref_type == 'tag' && startsWith(github.ref_name, 'v')` 判断正式 Release，使用 `startsWith(github.ref, 'refs/heads/release/')` 判断候选分支。

- [ ] **步骤 3：运行 YAML 静态解析**

运行：`ruby -e 'require "yaml"; YAML.load_file(".github/workflows/build-linux.yml")'`

预期：命令成功退出。

### 任务 2：为每个架构生成可验证的发布资产

**文件：**
- 修改：`.github/workflows/build-linux.yml`

- [ ] **步骤 1：在容器打包阶段生成校验文件和元数据**

在 ZIP 成功后生成同名 `.sha256` 和 `.metadata` 文件，内容包含目标架构、`GITHUB_REF`、`GITHUB_SHA`、构建镜像和包文件名。

- [ ] **步骤 2：上传 ZIP、校验和元数据**

将 `actions/upload-artifact@v4` 的 `path` 扩展为三个文件，保持每个架构独立的 Artifact，并设置 `if-no-files-found: error`。

- [ ] **步骤 3：给候选分支增加不可混淆的 Artifact 名称**

发布事件的 Artifact 名称包含目标架构、候选分支安全化名称和 commit SHA 短前缀；PR/main 构建保留现有稳定名称。

- [ ] **步骤 4：更新 verify job 下载逻辑**

让 verify job 使用 build job 输出的实际 Artifact 名称和 ZIP 名称，并在解压前执行 `sha256sum -c`；保留既有运行验证和 E2E 证据上传。

### 任务 3：增加正式 GitHub Release 发布 job

**文件：**
- 修改：`.github/workflows/build-linux.yml`

- [ ] **步骤 1：增加聚合下载 job**

创建 `release` job，依赖 `verify-linux`，只在 `push`、tag 类型和 `v` 前缀同时满足时运行；下载两个架构的 ZIP、SHA256 和 metadata。

- [ ] **步骤 2：生成 Release asset 目录和校验汇总**

将所有文件复制到单一目录，检查 ZIP 与校验文件存在，并生成按实际 ZIP 文件名计算的 `SHA256SUMS`。

- [ ] **步骤 3：创建 GitHub Release**

使用 `softprops/action-gh-release@v2`，配置 tag、自动生成 release notes 和全部 assets；仅该 job 使用 `contents: write`。

- [ ] **步骤 4：验证 tag-only 行为**

通过脚本确认 Release job 条件同时包含 tag 事件和 `v` 前缀，`release/**` 分支不创建 Release。

### 任务 4：编写服务器部署与绿灯测试文档

**文件：**
- 创建：`docs/deployment/release-artifacts.md`
- 修改：`README.md`

- [ ] **步骤 1：写明下载和校验命令**

提供 GitHub Release 和 Actions Artifact 两种入口，使用 `sha256sum -c tte-mrcp-rhel7-x86_64.zip.sha256` 校验，并说明 x86_64/AArch64 选择规则。

- [ ] **步骤 2：写明目录切换和运行环境**

记录 `/opt/tte-mrcp/releases/<version>` 解压、`current` 软链接、`LD_LIBRARY_PATH`、配置备份、`bin/unimrcpserver -r ...` 启动和停止方式；禁止覆盖运行目录。

- [ ] **步骤 3：写明服务检查和绿灯判定**

包含端口监听、`server.log` 的 profile/plugin 检查、fixture self-test、loopback fixture、`tools/stress/stress_test_improved.sh` 和 `tools/diagnostics/run_websocket_e2e.sh` 命令及成功标准，并注明真实外部 TTS endpoint 需要人工配置。

- [ ] **步骤 4：在 README 增加唯一入口**

在构建/运行章节添加指向部署文档的相对链接，并说明 Release tag、候选分支和绿灯测试关系。

### 任务 5：加入 workflow 静态回归检查并验证

**文件：**
- 创建：`tools/ci/check-release-workflow.sh`

- [ ] **步骤 1：实现静态检查脚本**

使用 `rg` 检查 `v*` tag、`release/**` 分支、`upload-artifact@v4`、`sha256sum`、`verify-linux` 依赖、tag-only Release 条件、Release asset 上传和部署文档链接；缺失关键约束时返回非零。

- [ ] **步骤 2：运行脚本与变更卫生检查**

运行：

```bash
bash tools/ci/check-release-workflow.sh
git diff --check
find tools -type f -name '*.sh' -print0 | xargs -0 bash -n
```

预期：全部成功。

- [ ] **步骤 3：重新索引代码图谱并记录结果**

运行 codebase-memory `index_repository`，随后查询 canonical TTS/ASR 节点和关键运行入口；workflow 与文档事实以文件内容补充记录。

## 自检结果

- 两种发布入口对应任务 1。
- ZIP、SHA256、metadata 和正式 Release 对应任务 2、3。
- 服务器部署、启动、配置、绿灯测试和外部服务边界对应任务 4。
- 静态检查、YAML、shell 和图谱验收对应任务 5。
- 未引入远程自动部署、真实生产呼叫或与本需求无关的公共库修改。
