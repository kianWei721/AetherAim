# AetherAim

<div align="center">

**FPS Accessibility Assistant — Adaptive mouse filtering for gamers with motor disabilities.**

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11-lightgrey.svg)](https://www.microsoft.com/windows)

</div>

---

## 项目背景

**残障玩家在 FPS 游戏中面临的困境：**

全世界有超过 1000 万人患有帕金森病和特发性震颤，还有更多人面临小脑性共济失调、肌肉痉挛、周围神经病变等运动障碍。对他们来说，FPS 游戏几乎是无法触及的——不是因为反应慢或策略差，而是因为手无法稳定地握住鼠标。

```
一个普通玩家的准星轨迹:
  ═══════════════════  (平滑)

一个手抖玩家的准星轨迹:
  ═╱╲╱╲══╱╲╱╲╱╲══╱╲  (充满 4-12Hz 高频抖动)

AetherAim 滤波后:
  ═══════════════════  (接近正常)
```

**AetherAim 不是什么黑科技**——它是 2012 年一篇 CHI 人机交互顶会论文 (Casiez, Roussel, Vogel) 的工程化实现。核心算法 "1€ Filter" 的核心洞察是：**低速时重滤波（去除抖动），高速时轻滤波（保留跟枪手感）**。

**AetherAim 不是外挂**——它不读取游戏内存，不分析游戏画面，不生成任何瞄准动作。它只做一件事：把你不想要的抖动滤掉。类比：降噪耳机滤掉环境噪音但保留人声；AetherAim 滤掉手抖但保留你的瞄准意图。

> 参考论文: Casiez, G., Roussel, N., & Vogel, D. (2012). *1€ Filter: A Simple Speed-based Low-pass Filter for Noisy Input in Interactive Systems*. CHI '12.

---

## 快速开始

- **用户快速体验** → [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md)
- **开发者构建** → [`docs/BUILDING.md`](docs/BUILDING.md)
- **贡献代码** → [`CONTRIBUTING.md`](CONTRIBUTING.md)
- **Pico 硬件方案** → [`pico_firmware/SHOPPING_LIST.md`](pico_firmware/SHOPPING_LIST.md)

---

## Features

| Feature | Description |
|---------|------------|
| **OneEuro Adaptive Filter** | Speed-adaptive low-pass — heavy smoothing at rest, minimal lag during flicks |
| **EMA Filter** | Simple exponential moving average — zero-overhead baseline |
| **Kalman Filter** | Self-tuning 1D estimator — noise-adaptive smoothing |
| **WH_MOUSE_LL Hook** | Global low-level mouse interception for desktop and legacy games |
| **Raw Input Hooking** | MinHook-based DLL injection for modern FPS (CS2, OW2, Apex) |
| **Calibration Wizard** | 4-stage guided assessment: tremor, overshoot, tracking, reaction |
| **User Profiles** | Per-game JSON profiles with auto-tuned filter parameters |
| **Real-time Plot** | Raw vs filtered mouse delta visualization (implot) |
| **Radar Chart** | 5-axis motor ability visualization |
| **System Tray** | Minimize to notification area with context menu |
| **Config Hot-reload** | Edit JSON config while running — changes apply instantly |
| **Sub-pixel Accumulation** | No cursor stiction at low speeds |
| **<5μs Latency** | Per-event processing time (target: well under 5ms budget) |

---

## Supported Games

### WH_MOUSE_LL (desktop cursor filtering)

Works with any game that uses standard Windows mouse input:

| Game | Status |
|------|--------|
| All desktop applications | Full support |
| Older FPS (pre-raw-input) | Full support |
| Games with raw input OFF | Full support |

### Raw Input DLL Injection

For modern FPS titles using `RegisterRawInputDevices`:

| Game | Executable | Anti-Cheat | Status |
|------|-----------|------------|--------|
| Counter-Strike 2 | `cs2.exe` | VAC | Use with caution |
| Overwatch 2 | `overwatch.exe` | None (optional) | Safe |
| Apex Legends | `r5apex.exe` | EAC | Community servers only |
| Call of Duty | `cod.exe` | Ricochet | Not recommended |
| Rainbow Six Siege | `RainbowSix.exe` | BattlEye | Not recommended |
| Quake Champions | `QuakeChampions.exe` | None | Safe |
| Splitgate 2 | `splitgate2.exe` | None | Safe |
| **Valorant** | `valorant.exe` | **Vanguard (kernel)** | **Blocked** |

> **Anti-Cheat Warning:** DLL injection and API hooking are detectable by anti-cheat systems. AetherAim is designed for accessibility in single-player, community servers, and games that permit input modification. Use in competitive matchmaking at your own risk. This tool is intended to be used with a valid disability certificate where required by local regulations.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     AetherAim.exe                         │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐ │
│  │  Config  │  │ Profiles │  │   GUI    │  │  System  │ │
│  │ Manager  │  │ Manager  │  │(DX11+Im) │  │   Tray   │ │
│  │  (JSON)  │  │  (JSON)  │  │  gui)    │  │          │ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────────┘ │
│       │             │             │                       │
│  ┌────┴─────────────┴─────────────┴──────────────────┐   │
│  │                InputProcessor                       │   │
│  │    (filter strategy selection + parameter push)     │   │
│  └────────────────────────┬──────────────────────────┘   │
│                           │                               │
│  ┌────────────────────────┴──────────────────────────┐   │
│  │              MouseHookManager                       │   │
│  │  WH_MOUSE_LL → delta calc → filter → SendInput     │   │
│  │  (OneEuro2D / EMA2D / Kalman2D)                     │   │
│  └────────────────────────────────────────────────────┘   │
│                           │                               │
│  ┌────────────────────────┴──────────────────────────┐   │
│  │              GameInjector                           │   │
│  │  SharedMemory → AetherAimHook.dll (injected)       │   │
│  └────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                 Game Process (cs2.exe / ...)              │
│                                                           │
│  game calls GetRawInputData()                             │
│       │                                                   │
│       ▼                                                   │
│  AetherAimHook.dll (MinHook)                              │
│       │                                                   │
│       ├─ Call original → get RAWINPUT                     │
│       ├─ Read filter params from shared memory            │
│       ├─ OneEuro2D::filter(dx, dy, dt)                    │
│       ├─ Modify RAWMOUSE.lLastX / lLastY                  │
│       └─ Return filtered data → game receives clean aim   │
└─────────────────────────────────────────────────────────┘
```

### Source Tree (~7750 lines, 32 files)

```
src/
├── main.cpp                         # Entry point
├── core/
│   ├── OneEuroFilter.{hpp,cpp}      # 1€ adaptive low-pass filter
│   ├── EMAFilter.hpp                # Exponential moving average
│   ├── KalmanFilter.hpp             # 1D linear Kalman filter
│   ├── MouseHookManager.{hpp,cpp}   # WH_MOUSE_LL hook + SendInput
│   └── InputProcessor.{hpp,cpp}     # Filter strategy orchestrator
├── config/
│   ├── GlobalAppConfig.hpp          # All configuration data structures
│   ├── ConfigManager.{hpp,cpp}      # JSON persistence + hot-reload
│   ├── ProfileData.hpp              # User ability profile types
│   └── ProfileManager.{hpp,cpp}     # Profile CRUD + stats analysis
├── gui/
│   ├── GUIApp.{hpp,cpp}             # ImGui DX11 main window
│   ├── Widgets.{hpp,cpp}            # Radar chart, sliders, badges
│   └── CalibrationWizard.{hpp,cpp}  # 4-stage motor assessment
├── hook/
│   ├── SharedMemory.hpp             # IPC protocol definition
│   └── AetherAimHook.cpp            # Injected DLL (MinHook)
├── injector/
│   └── GameInjector.{hpp,cpp}       # DLL injection/ ejection manager
└── utils/
    ├── Logger.{hpp,cpp}             # Thread-safe logging
    ├── HotkeyManager.{hpp,cpp}      # Hotkey polling + sleep resilience
    ├── SystemTray.{hpp,cpp}         # Notification area icon
    └── MathUtils.hpp                # Math + timing helpers
```

---

## Build

### Prerequisites

| Requirement | Version |
|------------|---------|
| Windows | 10 (21H2+) or 11 |
| Visual Studio | 2022 (17.4+) with MSVC v143 |
| CMake | 3.24+ |
| Git | Any recent version |

Dependencies are fetched automatically via CMake `FetchContent`:
- **Dear ImGui** v1.91.6 — GUI framework
- **implot** v0.18 — Real-time plotting
- **nlohmann/json** v3.11.3 — JSON serialization
- **MinHook** — API hooking library
- **Catch2** v3.7.1 — Test framework (optional)

### Build Steps

```powershell
# Clone the repository
git clone https://github.com/your-org/AetherAim.git
cd AetherAim

# Configure (Release build recommended for performance)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Output
#   build\src\Release\AetherAim.exe
#   build\src\Release\AetherAimHook.dll
```

### Build Options

```powershell
# With SQLite profile storage (optional)
cmake -B build -DAETHER_USE_SQLITE=ON

# Without tests
cmake -B build -DBUILD_TESTS=OFF

# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
```

---

## Usage

### First Run

1. **Run as Administrator** — AetherAim requires elevation for the `WH_MOUSE_LL` global mouse hook. If launched without elevation, it will prompt for UAC approval and re-launch itself.

2. **Toggle the filter** — Press `NumLock` or `F8` to enable/disable filtering. The status bar at the top of the GUI shows the current state:
   - `● FILTER ACTIVE` (green) — filtering is on
   - `○ FILTER PAUSED` (red) — filtering is off

3. **Calibrate** — Go to the **Profile** tab and click **Start Calibration** to run the 4-stage motor assessment. This auto-tunes the filter to your specific tremor pattern.

### GUI Tabs

| Tab | Purpose |
|-----|---------|
| **Monitor** | Real-time raw-vs-filtered plot + session statistics |
| **Profile** | Radar chart, manual assessment scores, calibration wizard |
| **Settings** | Filter type, parameter sliders, deadzone, hotkeys |
| **Games** | Raw Input injection for modern FPS titles |
| **About** | Version info, keyboard shortcuts, dev tool toggles |

### Hotkeys

| Key | Action |
|-----|--------|
| `NumLock` | Toggle filter on/off |
| `F8` | Toggle filter on/off (alternate) |
| `Alt+F4` | Exit application (or minimize to tray via `×` button) |

### System Tray

- Click `×` on the window → minimizes to notification area
- Right-click tray icon → context menu:
  - Show/Hide Window
  - Toggle Filter
  - Exit AetherAim
- Double-click tray icon → show/hide window

### Raw Input Games

1. Launch your game (e.g., CS2, Overwatch 2)
2. Go to the **Games** tab
3. Click **Refresh Scan** to detect running games
4. Click **Inject** next to your game
5. Filter parameters sync in real-time via shared memory
6. Click **Eject** to unload the DLL before closing the game

### Command Line

```
AetherAim.exe                          # Normal startup
AetherAim.exe /minimized               # Start minimized to tray
AetherAim.exe /profile my_tremor_cfg   # Load specific profile
```

---

## Configuration

All settings are stored in `assets/default_config.json` and can be edited via the GUI or directly:

```json
{
  "global": {
    "enabled": false,
    "hotkey": 144,        // VK_NUMLOCK
    "hotkeyAlt": 119      // VK_F8
  },
  "filter": {
    "type": "one_euro",
    "oneEuro": {
      "minCutoff": 1.2,   // Hz — lower = more smoothing
      "beta": 0.0,        // Speed adaptation — higher = less flick lag
      "dCutoff": 1.0,     // Hz — derivative smoothness
      "speedCoeff": 1.0   // Speed sensitivity multiplier
    }
  },
  "advanced": {
    "deadzonePx": 0,      // px — suppress sub-pixel tremor
    "maxDeltaPx": 100     // px — prevent coordinate-jump glitches
  }
}
```

### Filter Parameter Guide

| Parameter | Range | Effect |
|-----------|-------|--------|
| **Min Cutoff** | 0.01 – 10 Hz | Lower = heavier smoothing at rest. Start at 1.2 Hz, reduce for severe tremor. |
| **Beta** | 0 – 200 | Speed adaptation. 0 = fixed smoothing. High values (50+) = minimal lag during flick shots. |
| **D-Cutoff** | 0.1 – 5 Hz | Smoothness of the speed estimate. 1.0 Hz is standard. |
| **Speed Coeff** | 0.1 – 5 | Multiplier on detected speed. >1 amplifies responsiveness. |
| **Deadzone** | 0 – 50 px | Minimum movement per axis before filtering engages. |

---

## How It Works

### OneEuro Filter (1€ Filter)

The core algorithm comes from the CHI 2012 paper by Casiez, Roussel, and Vogel:

```
Step 1: Estimate derivative    dx = (raw - prevRaw) / dt
Step 2: Low-pass derivative    dxHat = α_d · dx + (1-α_d) · prevDx
Step 3: Adaptive cutoff        fc = minCutoff + β · |dxHat| · speedCoeff
Step 4: Low-pass signal        filtered = α(fc) · raw + (1-α(fc)) · prevFiltered
```

The key insight: at low speed (holding an angle), the cutoff stays low → heavy tremor suppression. During a fast flick, the cutoff rises with speed → minimal lag.

### Latency Budget

Per mouse event at 1000 Hz polling rate:

| Operation | Time |
|-----------|------|
| QPC read | ~0.02 μs |
| Delta calculation | ~0.01 μs |
| OneEuro filter (2D) | ~0.05 μs |
| SendInput syscall | ~2–5 μs |
| **Total per event** | **~3–5 μs** |

Target budget: <5000 μs (5ms). Actual: ~5 μs. **~1000× under budget**.

---

## FAQ

### Does this work with Valorant?

No. Valorant's Vanguard anti-cheat operates at kernel level and blocks both `OpenProcess` (preventing DLL injection) and user-mode API hooks. There is currently no workaround.

### Does VAC detect this?

VAC (Valve Anti-Cheat) primarily targets signature-based cheat detection. Accessibility tools using DLL injection have historically been tolerated, but there are no guarantees. Use in competitive CS2 matchmaking at your own risk.

### Why do I need to run as Administrator?

Windows 10/11 enforces User Interface Privilege Isolation (UIPI). Global low-level hooks (`WH_MOUSE_LL`) require the installing process to run at the same or higher integrity level as the target applications. Administrator elevation satisfies this requirement.

### Can I use this for general desktop use?

Yes. The WH_MOUSE_LL hook filters all mouse movement system-wide when enabled. This can help with general computer use for users with hand tremor.

### My anti-virus flagged AetherAimHook.dll — is it safe?

The DLL uses standard Windows API hooking techniques (MinHook) which are also used by malware. This is a false positive. The source code is available for inspection. You may need to add an exception in your AV software.

### How do I uninstall?

Delete the AetherAim directory. No registry changes are made except the optional auto-start entry (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\AetherAim`), which can be removed via `regedit` or by toggling auto-start off in the config before deleting.

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.

Third-party dependencies:
- Dear ImGui — MIT
- implot — MIT
- nlohmann/json — MIT
- MinHook — BSD 2-Clause

---

## Disclaimer

AetherAim is an accessibility tool designed to help gamers with medically recognized motor disabilities play FPS games. It is not a cheat or hack. However:

1. **Some anti-cheat systems may flag or block this software.** Always check the game's terms of service and accessibility policies.
2. **Use in competitive/ranked matchmaking may result in bans** in games with strict anti-cheat enforcement.
3. **This software does not provide aim assist, auto-aim, or aimbot functionality.** It only smooths existing mouse input.
4. **Users are responsible** for ensuring compliance with local regulations, game EULAs, and competitive integrity rules. In some jurisdictions, assistive technology use in gaming may require a documented disability.

---

*Built with C++20, Dear ImGui, and the OneEuro filter algorithm.*

*AetherAim — because everyone deserves a fair shot.*
