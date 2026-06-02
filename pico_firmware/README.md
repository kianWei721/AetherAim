# AetherAim Pico HID Proxy

物理硬件鼠标滤波器 — 适用于 **所有 FPS 游戏**，包括使用内核级反作弊的游戏（Valorant/Vanguard、三角洲行动/ACE、R6S/BattlEye）。

## 原理

```
物理鼠标 ──USB──► Raspberry Pi Pico ──USB──► 电脑
                    │
                    ├─ 接收原始鼠标 HID 数据包
                    ├─ OneEuro 自适应滤波
                    └─ 发送滤波后数据到电脑

电脑/反作弊系统看到的: 一个完全标准的 USB HID 鼠标
反作弊系统看不到:     Pico 芯片上运行的滤波算法
```

**为什么无法被反作弊检测:**
- Pico 输出的每个 USB 数据包都 100% 符合 HID 规范
- USB 协议栈没有"设备内部在做数学运算"的语义
- 不涉及任何电脑上的软件注入/hook/驱动
- 反作弊系统的 Ring 0 权限在这里毫无用处

## 物料清单 (¥40)

| 物料 | 价格 | 购买渠道 |
|------|------|---------|
| Raspberry Pi Pico | ¥25 | 淘宝/京东搜索"树莓派 Pico" |
| Micro-USB 母座转接板 | ¥3 | 淘宝"micro USB 母座 转接板" |
| 杜邦线 (母对母) ×4 | ¥2 | 淘宝/电子市场 |
| USB 数据线 ×2 | ¥10 | Micro-USB, 用于连接电脑和鼠标 |

> 如果你鼠标本来就是 Micro-USB 口的，可以用一根现成的 Micro-USB 线剪开焊接，省掉转接板。

## 硬件连接

```
                    Raspberry Pi Pico
                 ┌──────────────────────┐
                 │                      │
 鼠标            │  GPIO 0 (D+, 绿)     │
 ┌──────┐        │  GPIO 1 (D-, 白)     │
 │ USB  │────────│                      │
 │ 母座  │        │  3.3V (Pin 36) ─────┼─── 鼠标 VBUS (红, 供电)
 │      │        │  GND  (Pin 38) ──────┼─── 鼠标 GND  (黑)
 └──────┘        │                      │
                 │  Micro-USB ──────────┼─── 连接到电脑
                 │  (Pico 自带接口)      │
                 └──────────────────────┘
```

**引脚对应关系:**

| Pico 引脚 | USB 线颜色 | 用途 |
|----------|-----------|------|
| GPIO 0 | 绿 (D+) | 鼠标数据+ |
| GPIO 1 | 白 (D-) | 鼠标数据- |
| Pin 36 (3V3 OUT) | 红 (VBUS) | 鼠标供电 (5V→3.3V, 大多数鼠标在 3.3V 下可工作) |
| Pin 38 (GND) | 黑 (GND) | 共地 |

> **注意:** 如果鼠标在 3.3V 下不能工作（多数可以），需要外接 5V 供电。此时 VBUS 不接 Pico 3.3V，改接 Pico 的 VBUS (Pin 40, 5V)。

## 构建

```bash
# 1. 安装依赖
git clone https://github.com/raspberrypi/pico-sdk.git
export PICO_SDK_PATH=$(pwd)/pico-sdk

cd pico_firmware
mkdir -p lib
git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git lib/pico-pio-usb

# 2. 构建
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=$PICO_SDK_PATH
make -j4
```

## 烧录

1. **按住 Pico 上的 BOOTSEL 按钮**（白色小按钮）
2. 用 USB 线连接 Pico 到电脑
3. 松开 BOOTSEL 按钮
4. 电脑上会出现一个名为 `RPI-RP2` 的 U 盘
5. 将 `build/aether_pico.uf2` 复制到 RPI-RP2 盘
6. Pico 自动重启，开始运行固件

## 验证

烧录后：

1. **把鼠标插到 GPIO 0/1 连接的 USB 母座上**
2. Pico 通过 Micro-USB 连接到电脑
3. 电脑识别到"新 USB 鼠标"
4. 移动鼠标 → 光标移动（已经被滤波）

**LED 指示:**
- 慢闪 (250ms) — Pico 运行中，等待鼠标连接
- 快闪双闪 — 鼠标已连接，正常工作中

## 参数调整

目前参数硬编码在 `src/main.cpp` 顶部：

```cpp
static float FILTER_MIN_CUTOFF  = 1.2f;   // 降低 = 更平滑
static float FILTER_BETA        = 10.0f;  // 增大 = flick 时延迟更小
static float FILTER_D_CUTOFF    = 1.0f;
static float FILTER_SPEED_COEFF = 1.0f;
static int   DEADZONE_PX        = 0;
```

| 症状 | 参数调整 |
|------|---------|
| 手抖明显 (帕金森) | `FILTER_MIN_CUTOFF = 0.5`, `FILTER_BETA = 30` |
| 甩枪过头 (过冲) | `FILTER_BETA = 0`, `FILTER_SPEED_COEFF = 1.5` |
| 感觉延迟 | 增大 `FILTER_BETA`, 增大 `FILTER_MIN_CUTOFF` |
| 光标不跟手 | 减小 `FILTER_BETA`, 减小 `FILTER_MIN_CUTOFF` |

修改后重新 `make -j4` 并烧录。

## 延迟

| 环节 | 时间 |
|------|------|
| USB 帧周期 (1000Hz 鼠标) | ≤1ms |
| 滤波计算 (soft-float @ 133MHz) | ~6μs |
| USB 帧输出 (1000Hz) | ≤1ms |
| **总增加延迟** | **<2ms** |

> 大多数游戏玩家无法察觉到 <2ms 的额外延迟。
> 人眼到手指的神经延迟约为 150-200ms，2ms 仅为 1%。

## 下一步（改进方向）

- [ ] 串口 CLI: 电脑通过 USB 串口实时调整参数 (115200 baud)
- [ ] OLED 显示屏: 显示当前滤波参数和鼠标连接状态
- [ ] 编码器旋钮: 物理旋钮调整平滑强度
- [ ] 双鼠标支持: 同时连接两个鼠标（左手+右手）
- [ ] 升级到 Pico 2: 硬件 FPU 加速, 双 USB 控制器原生支持

## 许可证

MIT License — 与 AetherAim 主项目相同。
