# macOS 本机开发环境

本目录提供 macOS arm64 的可重复开发环境入口，不修改用户的全局 shell 配置。它只负责 macOS 开发机，不代表 Windows 或 Linux 的构建/生产验证已经完成。

```sh
./tools/dev/setup_macos.sh
. ./tools/dev/env-macos.sh
```

环境脚本会根据 Homebrew 前缀设置 `PATH`、`PKG_CONFIG_PATH`、`CPPFLAGS`、`LDFLAGS`、`CC` 和 `CXX`，避免依赖 Intel macOS 的 `/usr/local/opt` 路径。

配置完成后，优先使用 Autotools：

```sh
. ./tools/dev/env-macos.sh
./bootstrap
mkdir -p build/local
cd build/local
../../configure --prefix="$PWD/install"
make -j"$(sysctl -n hw.ncpu)"
make check
```

`tools/dev/Brewfile` 是依赖清单；`setup_macos.sh` 可以重复执行。若 Homebrew 网络或某个公式不可用，脚本会保留失败输出，不把未完成的依赖安装误报为构建成功。

当前 CMake 文件仍声明 2.8 兼容级别；CMake 4.x 配置时需显式允许旧策略：

```sh
cmake -S . -B /tmp/tte-mrcp-cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

Windows 开发使用仓库根目录的 Visual Studio solution 和 `.vcxproj`/`.props`；Linux 生产使用目标发行版的 Autotools/CMake 工具链。不要把本脚本中的 Homebrew 前缀、Apple clang 或 Darwin 参数复制到 Windows/Linux 的共享构建文件。
