# TTE-MRCP 仓库结构整理设计

## 目标

在不回退现有源码调试改动的前提下，把生成物、运行日志、操作系统元数据和历史备份移出活动源码树；将有效报告集中到 `docs/reports/`；用 `.gitignore` 阻止同类垃圾再次进入仓库。

## 现状证据

- 构建入口来自 `Makefile.am`、`configure.ac` 和各模块的 `Makefile.in`；当前工作树中的大量 `Makefile`、`config.status`、`.libs`、`.deps`、对象文件和动态库是本地构建生成物。
- `plugin/*.so`、`plugins/*/src/*.o`、`*.lo` 等已被纳入 Git，但不属于源码或构建规则，应从活动树移除并保留本地归档副本。
- `.DS_Store` 出现在根目录和插件目录，属于无业务价值的 macOS 元数据。
- `plugins/demo-{synth,recog}/src/bak` 只包含历史版本源码，不参与 `Makefile.am` 的编译清单。
- 两份分析报告是项目资产，应集中到文档报告目录，而不是与构建入口并列。

## 设计决策

1. 使用 `.archive/repo-cleanup-20260804/` 作为本次本地可恢复归档区，并将 `.archive/` 加入 `.gitignore`，避免归档副本重新污染源码图谱。
2. 归档并移出活动树的内容分为 `generated/`、`runtime/`、`legacy/`、`os-metadata/` 四类，保留原相对路径清单。
3. 删除 Git 中已跟踪但属于生成物或元数据的路径；不触碰 C/C++、配置源文件、测试脚本和用户当前修改。
4. 将两份 Markdown 报告移到 `docs/reports/`，保留中文文件名，并修正引用路径（当前代码中无构建依赖）。
5. `.gitignore` 只增加可由构建/运行生成的模式，不忽略源码、测试输入或部署配置。

## 验收标准

- 活动源码树不再包含 `.DS_Store`、`plugin/*.so`、对象/Libtool 产物、`.libs`、`.deps`、运行日志和 `src/bak`。
- `git status --short --ignored` 能明确区分用户源码改动、待归档删除项和被忽略的生成物。
- `autoreconf`/`./configure` 能重新生成 Makefile；至少执行配置语法检查和目标文件清单检查。
- 报告可从 `docs/reports/` 访问，`README.md` 现有压测入口保持不变。
- `.archive/repo-cleanup-20260804/归档清单.md` 记录每类文件的原路径和处理方式。
