# TTE-MRCP 本机环境与工具目录整理设计

## 目标

记录当前 macOS 本机的可用构建条件与阻断点，并把压测、诊断、Expect 集成脚本从根目录按职责归位。

## 目录设计

- `tools/stress/`：`stress_test.py`、`stress_test.sh`、`stress_test_improved.sh`、`stress_test_simple.sh`。
- `tools/diagnostics/`：`diagnose.sh` 及健康检查类工具。
- `tests/integration/`：`umc_test.exp` 等依赖已编译 UMC 的端到端脚本。
- `shell/`：服务启停、定时巡检和日志维护脚本，保持现有运维语义。
- `docs/reports/`：故障排查和架构报告。
- `.archive/`：本地归档，不进入版本库。

根目录不保留已迁移工具的重复脚本；压测、诊断和集成测试均通过职责目录中的 canonical 入口调用。

## 本机环境说明

README 增加当前机器的事实边界：macOS Darwin、可用 `make/gcc/clang`、当前未安装 CMake/autoreconf、完整构建还需要 APR/Sofia-SIP 等依赖；`configure --help` 和 shell 语法检查可执行，但不能把它们当成完整构建成功证明。

## 路径兼容

- `tools/stress/stress_test_improved.sh` 不依赖硬编码的 `/Users/wp/...` 默认根目录；默认使用脚本所在仓库根目录，仍支持 `-r` 覆盖。
- Expect 脚本路径改为 `tests/integration/umc_test.exp`。
- README 的示例统一使用新目录，不再提供根目录旧命令。

## 验收标准

- 新目录中的脚本具备执行权限，`bash -n`/`python3 -m py_compile` 通过。
- 活动文档、CI 和脚本均引用 canonical 工具路径，不引用已删除的根目录脚本。
- README 有本机环境和目录地图，明确区分“工具可用”“依赖未满足”“完整构建未验证”。
