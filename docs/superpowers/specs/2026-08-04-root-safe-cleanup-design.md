# TTE-MRCP 项目根目录安全整理设计

## 目标

在保留 Autotools/CMake 有效构建输入、源码、测试和兼容入口的前提下，继续收敛项目根目录中的明确备份文件和本地生成目录，并把它们放入现有 `.archive/repo-cleanup-20260804/` 归档区。

## 已批准范围

本次只处理两类确定不属于活动源码树的对象：

| 当前路径 | 归档路径 | 依据 |
|---|---|---|
| `configure~` | `.archive/repo-cleanup-20260804/legacy/root/configure~` | 编辑器/工具生成的备份副本 |
| `build/local/` | `.archive/repo-cleanup-20260804/generated/build/local/` | 本机 Autotools 配置、Makefile、`.deps`、对象文件和 Libtool 状态 |

移动保留文件内容、相对路径信息和原有权限；归档目录已被 `.gitignore` 忽略，便于本机恢复而不污染活动源码树。

## 明确保留

以下内容不在本次移动范围内：

- 根目录 `CMakeLists.txt`、`Makefile.am`、`Makefile.in`、`configure`、`configure.ac`、`aclocal.m4`、`bootstrap`。
- `build/` 根目录的 Autotools 源码输入，包括 `acmacros/`、`rules/`、`pkgconfig/`、辅助脚本和版本头文件。
- 根目录的 `stress_test*.sh`、`stress_test.py`、`diagnose.sh`、`umc_test.exp` 兼容包装。
- `libs/`、`modules/`、`plugins/`、`platforms/`、`conf/`、`data/`、`tests/`、`tools/`、`docs/` 和 `.vscode/`。

## 恢复方式

需要恢复时，将归档文件复制回其原路径：

```sh
cp .archive/repo-cleanup-20260804/legacy/root/configure~ ./configure~
cp -R .archive/repo-cleanup-20260804/generated/build/local ./build/local
```

恢复后重新执行脚本语法检查和 Autotools 配置检查；归档目录本身不作为构建输入。

## 验收标准

- 根目录不存在 `configure~`。
- 活动树不存在 `build/local/`，但 `build/` 源码输入保持存在。
- 两个归档目标均存在，并且归档清单包含原路径、归档路径和恢复说明。
- `.gitignore` 忽略 `configure~`，不会扩大到源码、配置或测试输入。
- Shell/Python 静态检查和旧路径扫描通过。
