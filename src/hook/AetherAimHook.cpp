// ============================================================================
// AetherAimHook.dll — Raw Input Filter DLL (injected into game process)
//
// PURPOSE:
//   Intercepts GetRawInputData / GetRawInputBuffer calls in the target
//   game process using MinHook. Filters RAWMOUSE lLastX/lLastY values
//   through the OneEuro adaptive filter before the game sees them.
//
// ARCHITECTURE:
//   Game calls GetRawInputData(hRawInput, RID_INPUT, ...)
//     → Our hook: call original, get RAWINPUT data
//     → If mouse movement: filter lLastX, lLastY
//     → Return filtered data to game
//
// PARAMETERS:
//   Read from named shared memory (Global\AetherAim_SharedMemory).
//   Updated by AetherAim.exe GUI in real-time.
//
// ANTI-CHEAT NOTICE:
//   DLL injection + API hooking is detectable by anti-cheat systems.
//   - VAC (CS2): historically tolerant of accessibility tools
//   - Vanguard (Valorant): WILL block injection at kernel level
//   - BattlEye / EAC: will detect; use at own risk
//   This tool is intended for accessibility use in single-player,
//   community servers, and games that permit input modification.
//
// BUILD:
//   Compiled as AetherAimHook.dll (64-bit, /MT static CRT, no dependencies
//   beyond kernel32.dll, user32.dll, and the shared memory objects).
// ============================================================================

#include <Windows.h>
#include <cstdint>
#include <cmath>

// MinHook
#include "MinHook.h"

// Our filter core
#include "core/OneEuroFilter.hpp"
#include "core/EMAFilter.hpp"
#include "core/KalmanFilter.hpp"

// Shared memory protocol
#include "hook/SharedMemory.hpp"

// ============================================================================
// Globals (file-scope, initialized once when DLL loads)
// ============================================================================
static aether::OneEuroFilter2D g_oneEuro2D;
static aether::EMAFilter2D     g_ema2D;
static aether::KalmanFilter2D  g_kalman2D;

// Shared memory
static HANDLE g_hMapFile     = nullptr;
static aether::ipc::SharedMemoryLayout* g_sharedMem = nullptr;

// Hook handles
static bool g_hooksInstalled = false;

// Timing
static LARGE_INTEGER g_qpcFreq{};
static LARGE_INTEGER g_lastQPC{};
static double        g_invFreq = 0.0;
static bool          g_firstEvent = true;

// Sub-pixel accumulators
static double g_fractX = 0.0;
static double g_fractY = 0.0;

// Function pointers for original API
typedef UINT (WINAPI* GetRawInputData_t)(
    HRAWINPUT hRawInput, UINT uiCommand,
    LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);

typedef UINT (WINAPI* GetRawInputBuffer_t)(
    PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader);

static GetRawInputData_t   Original_GetRawInputData   = nullptr;
static GetRawInputBuffer_t Original_GetRawInputBuffer = nullptr;

// ============================================================================
// Helper: compute dt from QPC
// ============================================================================
static double computeDt() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = static_cast<double>(now.QuadPart - g_lastQPC.QuadPart) * g_invFreq;
    g_lastQPC = now;

    // Clamp dt: if > 500ms, probably resumed from sleep or first event
    if (dt > 0.5 || dt < 1e-6) {
        dt = 0.008;  // Assume ~125Hz baseline
    }
    return dt;
}

// ============================================================================
// Helper: read current filter type from shared memory
// ============================================================================
static aether::ipc::IPC_FilterType getFilterType() {
    if (!g_sharedMem) return aether::ipc::IPC_FilterType::OneEuro;
    int32_t t = g_sharedMem->control.filterType.load(std::memory_order_acquire);
    return static_cast<aether::ipc::IPC_FilterType>(t);
}

// ============================================================================
// Helper: apply 2D filter based on current params in shared memory
// ============================================================================
static void filterMouseDelta(LONG& dx, LONG& dy, double dt) {
    if (!g_sharedMem) return;

    auto& ctrl = g_sharedMem->control;

    // ── Deadzone ──────────────────────────────────────────────────────
    int deadzone = ctrl.deadzonePx.load(std::memory_order_relaxed);
    if (deadzone > 0 && std::abs(dx) < deadzone && std::abs(dy) < deadzone) {
        dx = 0;
        dy = 0;
        return;
    }

    // ── MaxDelta clamp ─────────────────────────────────────────────────
    int maxDelta = ctrl.maxDeltaPx.load(std::memory_order_relaxed);
    double fdx = static_cast<double>(dx);
    double fdy = static_cast<double>(dy);
    fdx = std::clamp(fdx, -static_cast<double>(maxDelta), static_cast<double>(maxDelta));
    fdy = std::clamp(fdy, -static_cast<double>(maxDelta), static_cast<double>(maxDelta));

    // ── Filter dispatch ────────────────────────────────────────────────
    auto ft = getFilterType();

    // Update OneEuro params from shared memory (only for OneEuro path)
    if (ft == aether::ipc::IPC_FilterType::OneEuro) {
        auto& oe = ctrl.oneEuro;
        g_oneEuro2D.setMinCutoff(oe.minCutoff);
        g_oneEuro2D.setBeta(oe.beta);
        g_oneEuro2D.setDCutoff(oe.dCutoff);
        g_oneEuro2D.setSpeedCoeff(oe.speedCoeff);
    }

    switch (ft) {
    case aether::ipc::IPC_FilterType::OneEuro: {
        auto result = g_oneEuro2D.filter(fdx, fdy, dt);
        fdx = result.x;
        fdy = result.y;
        break;
    }
    case aether::ipc::IPC_FilterType::EMA: {
        g_ema2D.setAlpha(ctrl.ema.alpha);
        auto result = g_ema2D.filter(fdx, fdy);
        fdx = result.x;
        fdy = result.y;
        break;
    }
    case aether::ipc::IPC_FilterType::Kalman: {
        g_kalman2D.setParams(ctrl.kalman.processNoise,
                             ctrl.kalman.measurementNoise);
        auto result = g_kalman2D.filter(fdx, fdy);
        fdx = result.x;
        fdy = result.y;
        break;
    }
    case aether::ipc::IPC_FilterType::None:
    default:
        break;  // passthrough
    }

    // ── Sub-pixel accumulation ─────────────────────────────────────────
    if (ctrl.subPixelAccum.load(std::memory_order_relaxed)) {
        double totalX = fdx + g_fractX;
        double totalY = fdy + g_fractY;
        LONG intX = static_cast<LONG>(totalX);
        LONG intY = static_cast<LONG>(totalY);
        g_fractX = totalX - static_cast<double>(intX);
        g_fractY = totalY - static_cast<double>(intY);
        dx = intX;
        dy = intY;
    } else {
        dx = static_cast<LONG>(std::round(fdx));
        dy = static_cast<LONG>(std::round(fdy));
    }

    // ── Update stats ───────────────────────────────────────────────────
    auto& stats = g_sharedMem->stats;
    stats.totalEvents.fetch_add(1, std::memory_order_relaxed);
    stats.filteredEvents.fetch_add(1, std::memory_order_relaxed);

    // Exponential moving average of speed
    double speed = std::sqrt(dx * dx + dy * dy) / std::max(dt, 0.001);
    double prevRaw = stats.avgRawSpeed.load(std::memory_order_relaxed);
    stats.avgRawSpeed.store(prevRaw * 0.95 + speed * 0.05, std::memory_order_relaxed);
}

// ============================================================================
// Hook: GetRawInputData
//
// Signature:
//   UINT GetRawInputData(
//     HRAWINPUT hRawInput,
//     UINT      uiCommand,    // RID_INPUT, RID_HEADER
//     LPVOID    pData,        // RAWINPUT*
//     PUINT     pcbSize,
//     UINT      cbSizeHeader  // sizeof(RAWINPUTHEADER)
//   );
//
// The game calls this after receiving WM_INPUT to retrieve the raw
// mouse/keyboard/HID data. We let the original function fill the
// RAWINPUT structure, then modify the mouse lLastX/lLastY fields.
// ============================================================================
static UINT WINAPI Hook_GetRawInputData(
    HRAWINPUT hRawInput,
    UINT      uiCommand,
    LPVOID    pData,
    PUINT     pcbSize,
    UINT      cbSizeHeader)
{
    // Call the original first — it fills pData with the raw input
    UINT result = Original_GetRawInputData(
        hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);

    // Only filter when:
    //   1. The call succeeded (result > 0)
    //   2. We're retrieving input data (RID_INPUT), not just the header
    //   3. Filtering is enabled in shared memory
    //   4. pData is valid
    if (result == UINT(-1) || uiCommand != RID_INPUT || !pData || !g_sharedMem)
        return result;

    if (!g_sharedMem->control.filterEnabled.load(std::memory_order_acquire))
        return result;

    RAWINPUT* raw = static_cast<RAWINPUT*>(pData);
    if (raw->header.dwType != RIM_TYPEMOUSE)
        return result;

    // Only filter relative movement (MOUSE_MOVE_RELATIVE)
    // Skip absolute movement (MOUSE_MOVE_ABSOLUTE) — typically from
    // touchscreens or pen input, not relevant for FPS gaming.
    RAWMOUSE& mouse = raw->data.mouse;

    // Check if this is mouse movement (lLastX or lLastY non-zero)
    if (mouse.lLastX == 0 && mouse.lLastY == 0)
        return result;

    // Compute dt
    double dt = computeDt();

    // Filter the mouse delta
    LONG dx = mouse.lLastX;
    LONG dy = mouse.lLastY;
    filterMouseDelta(dx, dy, dt);
    mouse.lLastX = dx;
    mouse.lLastY = dy;

    return result;
}

// ============================================================================
// Hook: GetRawInputBuffer
//
// Less commonly used than GetRawInputData, but some game engines use it
// for batch reading of raw input messages. Same filtering logic, applied
// to each RAWINPUT entry in the buffer.
// ============================================================================
static UINT WINAPI Hook_GetRawInputBuffer(
    PRAWINPUT pData,
    PUINT     pcbSize,
    UINT      cbSizeHeader)
{
    UINT result = Original_GetRawInputBuffer(pData, pcbSize, cbSizeHeader);

    if (result == UINT(-1) || !pData || !g_sharedMem)
        return result;

    if (!g_sharedMem->control.filterEnabled.load(std::memory_order_acquire))
        return result;

    // GetRawInputBuffer returns the number of RAWINPUT structures written.
    // We iterate through them and filter each mouse entry.
    UINT count = result;
    RAWINPUT* raw = pData;
    double dt = computeDt();

    for (UINT i = 0; i < count; ++i) {
        if (raw->header.dwType == RIM_TYPEMOUSE) {
            if (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0) {
                LONG dx = raw->data.mouse.lLastX;
                LONG dy = raw->data.mouse.lLastY;
                filterMouseDelta(dx, dy, dt);
                raw->data.mouse.lLastX = dx;
                raw->data.mouse.lLastY = dy;
            }
        }
        // Advance to next RAWINPUT structure
        // RAWINPUT is variable-size; the next entry starts after
        // RAWINPUTHEADER.dwSize bytes from the current entry
        raw = reinterpret_cast<RAWINPUT*>(
            reinterpret_cast<BYTE*>(raw) + raw->header.dwSize);
    }

    return result;
}

// ============================================================================
// Shared memory initialization
// ============================================================================
static bool openSharedMemory() {
    // Try to open the named file mapping created by AetherAim.exe
    g_hMapFile = OpenFileMappingW(
        FILE_MAP_READ | FILE_MAP_WRITE,
        FALSE,
        aether::ipc::SHARED_MEMORY_NAME
    );

    if (!g_hMapFile) {
        // Main process might not have created the mapping yet.
        // The DLL can still function with default parameters.
        return false;
    }

    g_sharedMem = static_cast<aether::ipc::SharedMemoryLayout*>(
        MapViewOfFile(g_hMapFile, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));

    if (!g_sharedMem) {
        CloseHandle(g_hMapFile);
        g_hMapFile = nullptr;
        return false;
    }

    // Validate magic number
    if (g_sharedMem->control.magic != aether::ipc::SHARED_MEMORY_MAGIC) {
        UnmapViewOfFile(g_sharedMem);
        CloseHandle(g_hMapFile);
        g_sharedMem = nullptr;
        g_hMapFile = nullptr;
        return false;
    }

    return true;
}

// ============================================================================
// Hook installation
// ============================================================================
static bool installHooks() {
    // Initialize MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        return false;
    }

    // Hook GetRawInputData (user32.dll)
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        auto pGetRawInputData = reinterpret_cast<GetRawInputData_t>(
            GetProcAddress(hUser32, "GetRawInputData"));
        if (pGetRawInputData) {
            status = MH_CreateHook(
                reinterpret_cast<LPVOID>(pGetRawInputData),
                reinterpret_cast<LPVOID>(Hook_GetRawInputData),
                reinterpret_cast<LPVOID*>(&Original_GetRawInputData)
            );
            if (status != MH_OK) {
                MH_Uninitialize();
                return false;
            }
        }
    }

    // Hook GetRawInputBuffer (user32.dll)
    if (hUser32) {
        auto pGetRawInputBuffer = reinterpret_cast<GetRawInputBuffer_t>(
            GetProcAddress(hUser32, "GetRawInputBuffer"));
        if (pGetRawInputBuffer) {
            status = MH_CreateHook(
                reinterpret_cast<LPVOID>(pGetRawInputBuffer),
                reinterpret_cast<LPVOID>(Hook_GetRawInputBuffer),
                reinterpret_cast<LPVOID*>(&Original_GetRawInputBuffer)
            );
            if (status != MH_OK) {
                MH_Uninitialize();
                return false;
            }
        }
    }

    // Enable all hooks
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        MH_Uninitialize();
        return false;
    }

    g_hooksInstalled = true;
    return true;
}

// ============================================================================
// Hook removal
// ============================================================================
static void removeHooks() {
    if (g_hooksInstalled) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_hooksInstalled = false;
    }

    if (g_sharedMem) {
        UnmapViewOfFile(g_sharedMem);
        g_sharedMem = nullptr;
    }

    if (g_hMapFile) {
        CloseHandle(g_hMapFile);
        g_hMapFile = nullptr;
    }
}

// ============================================================================
// Init thread — spawned from DllMain after loader lock is released
// ============================================================================
static DWORD WINAPI InitThread(LPVOID /*param*/) {
    // Open shared memory (non-fatal if not available)
    openSharedMemory();

    // Initialize timing
    QueryPerformanceFrequency(&g_qpcFreq);
    g_invFreq = 1.0 / static_cast<double>(g_qpcFreq.QuadPart);
    QueryPerformanceCounter(&g_lastQPC);

    // Install API hooks
    installHooks();

    // The hooks are now active. This thread can exit — the hooks
    // will continue to intercept calls on whatever thread the game
    // calls GetRawInputData from.
    return 0;
}

// ============================================================================
// DllMain — called when DLL is loaded/unloaded
//
// IMPORTANT: DllMain runs under the loader lock. We must NOT:
//   - Call LoadLibrary / FreeLibrary
//   - Wait on synchronization objects
//   - Create a thread and wait for it
// We CAN safely call CreateThread (the thread starts AFTER DllMain returns).
// ============================================================================
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpReserved*/) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        // Disable thread attach/detach notifications for performance
        DisableThreadLibraryCalls(hinstDLL);

        // Spawn initialization thread — this runs AFTER DllMain returns,
        // so the loader lock is released and we can safely call MinHook.
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        // Clean up hooks
        removeHooks();
        break;
    }

    return TRUE;
}
