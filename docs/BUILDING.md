# 构建指南

## Windows 桌面程序 (AetherAim.exe + AetherAimHook.dll)

### 依赖

| 软件 | 版本 | 用途 |
|------|------|------|
| Windows | 10 21H2+ 或 11 | 运行环境 |
| Visual Studio | 2022 (17.4+) | MSVC 编译器 |
| CMake | 3.24+ | 构建系统 |
| Git | 任意 | 下载源码和依赖 |

> 依赖库 (Dear ImGui, implot, nlohmann/json, MinHook, Catch2) 在 CMake 配置时通过 FetchContent 自动下载，无需手动安装。

### 构建 (Release)

打开 **Developer Command Prompt for VS 2022**:

```powershell
cd AetherAim
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

产物:

```
build\src\Release\AetherAim.exe       ← 主程序
build\src\Release\AetherAimHook.dll   ← Raw Input 注入 DLL
```

### 构建 (Debug)

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

Debug 构建包含调试符号，滤波器性能会显著下降（不优化），仅用于开发调试。

### 运行测试

```powershell
# 必须先用 Release 或 Debug 构建
ctest --test-dir build -C Release

# 或逐个运行
build\tests\Release\test_filters.exe
build\tests\Release\test_config.exe
build\tests\Release\test_profile.exe
build\tests\Release\test_ringbuffer.exe
```

### 性能基准

```powershell
cmake --build build --config Release --target benchmark
build\tests\Release\benchmark.exe
```

### 可选开关

```powershell
# 关闭测试
cmake -B build -DBUILD_TESTS=OFF

# 启用 SQLite 支持 (需手动下载 sqlite3.c)
cmake -B build -DAETHER_USE_SQLITE=ON

# 使用 clang-cl 编译器
cmake -B build -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
```

### 添加自定义图标

把 `icon.ico` 放到 `assets/` 下，CMake 会自动检测并嵌入：

```
assets/
├── icon.ico         ← 你的图标 (256x256, .ico 格式)
├── app.rc           ← 资源文件 (已配置)
└── default_config.json
```

### 常见问题

**Q: cmake 找不到编译器?**
确保从 "Developer Command Prompt for VS 2022" 启动，而不是普通 PowerShell。

**Q: FetchContent 下载失败?**
可能是网络问题。设置代理后重试，或手动下载放到 `build/_deps/` 目录。

**Q: 链接错误 (LNK2019)?**
检查 CMake 版本 >= 3.24。

---

## Pico 固件 (aether_pico.uf2)

### 依赖

```bash
# 安装 ARM GCC 交叉编译器
# Ubuntu/Debian:
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential

# macOS:
brew install arm-none-eabi-gcc cmake
```

### 下载 SDK

```bash
git clone --depth 1 --branch 2.1.0 \
  https://github.com/raspberrypi/pico-sdk.git
export PICO_SDK_PATH=$(pwd)/pico-sdk

cd AetherAim/pico_firmware
mkdir -p lib
git clone --depth 1 \
  https://github.com/sekigon-gonnoc/Pico-PIO-USB.git \
  lib/pico-pio-usb
```

### 构建

```bash
cd pico_firmware
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

产物: `build/aether_pico.uf2`

### 自动构建 (GitHub Actions)

推送到 GitHub 后，`.github/workflows/build_pico_firmware.yml` 自动编译 `.uf2` 并上传到 Actions Artifact 和 Release。

---

## 项目结构

```
AetherAim/
├── src/                     # Windows 桌面程序 (C++20)
│   ├── core/                #   滤波器核心 (OneEuro/EMA/Kalman)
│   ├── config/              #   配置管理 (JSON)
│   ├── gui/                 #   GUI (Dear ImGui DX11)
│   ├── hook/                #   Raw Input DLL (MinHook)
│   ├── injector/            #   进程注入管理
│   ├── utils/               #   工具 (日志/热键/托盘)
│   └── main.cpp             #   入口
├── tests/                   # 测试 + 基准
├── pico_firmware/           # Pico 固件 (C++)
│   └── src/
│       ├── main.cpp         #   双核 USB Host + Device
│       ├── one_euro_filter.hpp  # 嵌入式滤波器
│       └── serial_console.hpp   # 串口控制台
├── assets/                  # 配置 + 资源
├── docs/                    # 文档
└── CMakeLists.txt           # 根 CMake
```
