// ============================================================================
// AetherAim — Entry Point
//
// Startup sequence:
//   1. Single-instance check (CreateMutex — prevent double-launch)
//   2. Admin elevation check (required for WH_MOUSE_LL)
//      → If not admin: prompt, then ShellExecuteEx("runas") to re-launch
//   3. Parse command line: /minimized, /profile <name>
//   4. Init logging
//   5. Load config (JSON → ConfigManager)
//   6. Load user profile (ProfileManager)
//   7. Init InputProcessor + apply config/profile params
//   8. Init HotkeyManager (NumLock / F8 toggle)
//   9. Init GUI (ImGui DX11 window)
//  10. Init SystemTray (notification area icon + context menu)
//  11. Install WH_MOUSE_LL hook
//  12. Run GUI main loop (blocks until exit)
//  13. Cleanup: tray, hook, hotkey, save profile+config
// ============================================================================

#include <Windows.h>
#include <string>
#include <shellapi.h>     // CommandLineToArgvW
#include <memory>

#include "utils/Logger.hpp"
#include "utils/HotkeyManager.hpp"
#include "utils/SystemTray.hpp"
#include "core/InputProcessor.hpp"
#include "core/OneEuroFilter.hpp"
#include "config/GlobalAppConfig.hpp"
#include "config/ConfigManager.hpp"
#include "config/ProfileData.hpp"
#include "config/ProfileManager.hpp"
#include "injector/GameInjector.hpp"
#include "gui/GUIApp.hpp"

using namespace aether;

// ============================================================================
// Constants
// ============================================================================
static constexpr const wchar_t* MUTEX_NAME        = L"Global\\AetherAim_SingleInstance";
static constexpr const wchar_t* WINDOW_CLASS_NAME = L"AetherAimGUI";
static constexpr const wchar_t* WINDOW_TITLE      = L"AetherAim — FPS Accessibility Assistant";

// ============================================================================
// Admin elevation
// ============================================================================
static bool isRunningAsAdmin() {
    BOOL isElevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev{};
        DWORD size = sizeof(elev);
        if (GetTokenInformation(hToken, TokenElevation, &elev, size, &size)) {
            isElevated = elev.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return isElevated;
}

// Returns true if re-launch was initiated (caller should exit current process)
static bool relaunchAsAdmin() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Get command line from current process
    std::wstring cmdLine = GetCommandLineW();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = L"runas";       // Triggers UAC elevation
    sei.lpFile       = exePath;
    sei.lpParameters = cmdLine.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            // User declined UAC prompt
            MessageBoxW(nullptr,
                L"AetherAim requires Administrator privileges to install the\n"
                L"WH_MOUSE_LL mouse hook. The application cannot run without\n"
                L"elevation.\n\n"
                L"Please accept the UAC prompt to continue.",
                L"AetherAim — Elevation Required",
                MB_ICONWARNING | MB_OK);
        }
        return false;
    }

    // Successfully launched elevated process — wait briefly for it to start,
    // then exit this non-elevated instance.
    WaitForSingleObject(sei.hProcess, 2000);
    CloseHandle(sei.hProcess);
    return true;
}

// ============================================================================
// Single-instance check
// ============================================================================
static HANDLE g_hMutex = nullptr;

static bool acquireSingleInstanceLock() {
    g_hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (!g_hMutex) return false;

    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        // Another instance is running — bring its window to front
        HWND hExisting = FindWindowW(WINDOW_CLASS_NAME, WINDOW_TITLE);
        if (hExisting) {
            ShowWindow(hExisting, SW_SHOW);
            SetForegroundWindow(hExisting);
        }
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
        return false;
    }
    return true;
}

static void releaseSingleInstanceLock() {
    if (g_hMutex) {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
    }
}

// ============================================================================
// Command-line parsing
// ============================================================================
struct CommandLineArgs {
    bool        startMinimized = false;
    std::string profileName;
};

static CommandLineArgs parseCommandLine() {
    CommandLineArgs args;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return args;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg(argv[i]);

        if (arg == L"/minimized" || arg == L"-minimized" || arg == L"--minimized") {
            args.startMinimized = true;
        } else if ((arg == L"/profile" || arg == L"-profile" || arg == L"--profile")
                   && i + 1 < argc) {
            // Convert wide string to UTF-8
            int len = WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                std::string profileName(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, &profileName[0], len, nullptr, nullptr);
                args.profileName = profileName;
            }
            ++i;  // Skip next arg
        }
    }
    LocalFree(argv);
    return args;
}

// ============================================================================
// FilterType parsing
// ============================================================================
static FilterType parseFilterType(const std::string& s) {
    if (s == "ema")      return FilterType::EMA;
    if (s == "kalman")   return FilterType::Kalman;
    if (s == "none")     return FilterType::None;
    return FilterType::OneEuro;
}

// ============================================================================
// Registry-based auto-start helper
// ============================================================================
static bool setAutoStart(bool enable) {
    HKEY hKey;
    LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey
    );
    if (status != ERROR_SUCCESS) return false;

    if (enable) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring value = std::wstring(L"\"") + exePath + L"\" /minimized";
        status = RegSetValueExW(hKey, L"AetherAim", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(hKey, L"AetherAim");
    }
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

// ============================================================================
// WinMain
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // ── 1. Single-instance lock ──────────────────────────────────────
    if (!acquireSingleInstanceLock()) {
        return 0;  // Another instance already running
    }

    // ── 2. Admin elevation ───────────────────────────────────────────
    if (!isRunningAsAdmin()) {
        if (relaunchAsAdmin()) {
            releaseSingleInstanceLock();
            return 0;  // Elevated process launched, exit this one
        }
        releaseSingleInstanceLock();
        return 1;  // User declined elevation
    }

    // ── 3. Parse command line ────────────────────────────────────────
    auto cmdArgs = parseCommandLine();

    // ── 4. Init logging ──────────────────────────────────────────────
    Logger::instance().setMinLevel(LogLevel::Debug);
    LOG_INFO("══════════════════════════════════════════");
    LOG_INFO("AetherAim v1.0.0 starting...");
    LOG_INFO("  Elevated: yes | PID: {}", GetCurrentProcessId());

    // ── 5. Load config ───────────────────────────────────────────────
    ConfigManager config("assets/default_config.json");
    if (!config.load()) {
        LOG_WARN("Config not found or corrupt — using defaults");
    }

    const auto& cfg = config.config();

    // ── 6. Load user profile ─────────────────────────────────────────
    std::string activeProfile = cmdArgs.profileName.empty()
        ? cfg.profiles.activeProfile
        : cmdArgs.profileName;

    ProfileManager profiles(cfg.profiles.profileDir);
    profiles.loadProfile(activeProfile);
    profiles.startSession();

    LOG_INFO("Active profile: '{}' (game: {})",
             profiles.active().name, profiles.active().game);

    // ── 7. Init InputProcessor ───────────────────────────────────────
    InputProcessor input;

    // Apply config defaults first
    {
        InputProcessor::FilterProfile fp;
        fp.type       = parseFilterType(cfg.filter.type);
        fp.minCutoff  = cfg.filter.oneEuro.minCutoff;
        fp.beta       = cfg.filter.oneEuro.beta;
        fp.dCutoff    = cfg.filter.oneEuro.dCutoff;
        fp.speedCoeff = cfg.filter.oneEuro.speedCoeff;
        fp.emaAlpha   = cfg.filter.ema.alpha;
        fp.kProcessNoise     = cfg.filter.kalman.processNoise;
        fp.kMeasurementNoise = cfg.filter.kalman.measurementNoise;
        fp.kInitialError     = cfg.filter.kalman.initialError;
        fp.deadzone   = cfg.advanced.deadzonePx;
        fp.maxDelta   = cfg.advanced.maxDeltaPx;
        input.applyProfile(fp);
    }

    // Override with profile-derived params (if calibrated)
    if (profiles.active().recommendedBeta > 0.001) {
        profiles.applyToProcessor(input);
        LOG_INFO("Profile-derived filter parameters applied");
    }

    // Apply initial enabled state from config
    input.setEnabled(cfg.global.enabled);

    // ── 8. Init HotkeyManager ────────────────────────────────────────
    HotkeyManager hotkeys;
    hotkeys.configure(
        cfg.global.hotkey,
        cfg.global.hotkeyAlt,
        [&](bool toggled) {
            input.setEnabled(toggled);
            config.setEnabled(toggled);
            LOG_INFO("Filter toggled via hotkey: {}",
                     toggled ? "ON" : "OFF");
        }
    );
    hotkeys.start();

    // If config says enabled, sync initial hotkey state
    if (cfg.global.enabled) {
        hotkeys.setToggled(true);
    }

    // ── 9. Init GUI ──────────────────────────────────────────────────
    GUIApp gui;
    SystemTray tray;
    GameInjector injector;

    // Create shared memory for Raw Input hook DLL communication
    injector.createSharedMemory();

    if (!gui.init(&config, &profiles, &input, &tray, &injector)) {
        LOG_ERROR("Failed to initialize GUI");
        MessageBoxW(nullptr,
            L"Failed to initialize the DirectX 11 renderer.\n"
            L"Please ensure your GPU supports DX11 Feature Level 11.0.",
            L"AetherAim — Renderer Error",
            MB_ICONERROR | MB_OK);
        hotkeys.stop();
        input.stop();
        releaseSingleInstanceLock();
        return 1;
    }

    // ── 10. Init System Tray ─────────────────────────────────────────
    // Use the GUI window handle for tray messages
    HWND hGuiWnd = FindWindowW(WINDOW_CLASS_NAME, WINDOW_TITLE);
    if (hGuiWnd) {
        tray.create(hGuiWnd, GUIApp::WM_TRAY_CALLBACK,
                    L"AetherAim — FPS Accessibility Assistant");
    }

    // Update tray tooltip with initial state
    std::wstring trayTooltip = L"AetherAim — Filter: ";
    trayTooltip += input.isEnabled() ? L"ON" : L"OFF";
    trayTooltip += L" | Profile: ";
    // Convert profile name to wide
    int len = MultiByteToWideChar(CP_UTF8, 0, profiles.active().name.c_str(), -1, nullptr, 0);
    if (len > 0) {
        std::wstring wname(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, profiles.active().name.c_str(), -1, &wname[0], len);
        trayTooltip += wname;
    }
    tray.setTooltip(trayTooltip);

    // Show first-run balloon if this looks like first launch
    if (!cfg.global.runOnStartup) {
        tray.showBalloon(
            L"AetherAim is Ready",
            L"Press NumLock or F8 to toggle mouse filtering.\n"
            L"Right-click this icon for options.",
            NIIF_INFO
        );
    }

    // ── 11. Install mouse hook ───────────────────────────────────────
    if (!input.start()) {
        LOG_ERROR("Failed to install WH_MOUSE_LL hook");
        MessageBoxW(nullptr,
            L"Failed to install the WH_MOUSE_LL mouse hook.\n\n"
            L"Possible causes:\n"
            L"  - Another application is using a low-level mouse hook\n"
            L"  - Antivirus software is blocking hook installation\n"
            L"  - Insufficient system resources\n\n"
            L"Try restarting your computer and running AetherAim first.",
            L"AetherAim — Hook Installation Failed",
            MB_ICONERROR | MB_OK);
        tray.destroy();
        hotkeys.stop();
        releaseSingleInstanceLock();
        return 1;
    }
    LOG_INFO("WH_MOUSE_LL hook installed successfully");

    // ── 12. Auto-start ───────────────────────────────────────────────
    if (cfg.global.runOnStartup) {
        setAutoStart(true);
    }

    // ── 13. Minimize to tray if requested ────────────────────────────
    if (cmdArgs.startMinimized || cfg.global.startMinimized) {
        gui.hideWindow();
        LOG_INFO("Started minimized to system tray");
    }

    // ── 14. Main loop (blocks until GUI closes) ──────────────────────
    LOG_INFO("Entering main loop");
    gui.run();

    // ── 15. Cleanup ──────────────────────────────────────────────────
    LOG_INFO("Shutting down...");

    tray.destroy();
    hotkeys.stop();
    input.stop();

    profiles.recomputeParameters();
    profiles.endSession();
    config.save();

    // Save window position for next launch
    // (handled in GUIApp cleanup — reads config.gui before destroying window)

    releaseSingleInstanceLock();

    LOG_INFO("AetherAim shutdown complete");
    LOG_INFO("══════════════════════════════════════════");
    return 0;
}
