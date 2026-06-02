#pragma once
// ============================================================================
// MouseHookManager.hpp — WH_MOUSE_LL Global Hook Manager
//
// RESPONSIBILITY:
//   1. Install / uninstall WH_MOUSE_LL global low-level mouse hook
//   2. Intercept WM_MOUSEMOVE, compute relative delta (dx, dy)
//   3. Route raw delta through the active filter (OneEuro / EMA / Kalman)
//   4. Suppress original mouse event (return 1) and re-inject filtered
//      movement via SendInput
//   5. Provide callbacks for GUI real-time plotting
//
// CRITICAL LATENCY REQUIREMENTS:
//   - The hook callback (lowLevelMouseProc) executes on the message-pump
//     thread at IRQL dispatch level for mouse events.
//   - ALL work inside the hook proc MUST complete in << 5ms to avoid
//     stuttering in other applications (browser, OBS, Discord).
//   - NO heap allocation (new/malloc), NO lock contention, NO I/O.
//   - QPC for timing (sub-μs, ~20 cycles), atomics for state flags.
//
// ARCHITECTURE NOTE — Raw Input Limitation:
//   WH_MOUSE_LL intercepts Windows cursor movement messages. Modern FPS
//   games that use the Raw Input API (RegisterRawInputDevices) bypass
//   the Windows cursor entirely — they read mouse data directly from
//   the HID driver stack. For these games, WH_MOUSE_LL filtering will
//   NOT affect in-game aim.
//
//   To support raw-input games, the hook layer is designed to be
//   swappable. The filter core (OneEuroFilter2D, etc.) is agnostic
//   to the capture mechanism and can be reused with:
//     - A kernel-level mouse filter driver (WDF / KMDF)
//     - A user-mode raw input hook via SetWindowsHookEx + RAWINPUT
//     - A virtual HID device
//   This is planned for a future release.
//
// THREAD MODEL:
//   - Hook proc:       called on installer thread (GUI main thread)
//   - Hotkey polling:  dedicated thread (HotkeyManager)
//   - GUI rendering:   main thread (GUIApp::run)
//   - Config save:     main thread (on shutdown)
//   All cross-thread communication uses std::atomic (lock-free).
// ============================================================================

#include <Windows.h>
#include <functional>
#include <atomic>
#include <array>
#include "OneEuroFilter.hpp"
#include "EMAFilter.hpp"
#include "KalmanFilter.hpp"

namespace aether {

// ── Filter type enumeration ───────────────────────────────────────────
enum class FilterType : uint8_t {
    OneEuro = 0,
    EMA     = 1,
    Kalman  = 2,
    None    = 3   // passthrough — raw delta, no filtering (debug)
};

// ── Raw delta from hook (before filtering) ────────────────────────────
struct alignas(16) RawDelta {
    LONG   dx;          // relative X movement (pixels)
    LONG   dy;          // relative Y movement (pixels)
    DWORD  timestamp;   // GetMessageTime() — system tick (ms granularity)
    double dt;          // QPC delta (seconds, sub-μs precision)
};

// ── Filtered output (after processing) ────────────────────────────────
struct alignas(16) FilteredDelta {
    double fx;          // filtered X
    double fy;          // filtered Y
    double rawSpeed;    // 2D speed of raw input (px/s)
    double filtSpeed;   // 2D speed of filtered output (px/s)
    double dt;          // same dt as input
};

// ============================================================================
// MouseHookManager
// ============================================================================
class MouseHookManager {
public:
    // ── Callback types ─────────────────────────────────────────────────
    // WARNING: These execute on the hook thread. Keep them FAST.
    // No heap allocation, no I/O, no blocking. Ideal: just push to a
    // lock-free ring buffer for the GUI thread to consume later.
    using PreFilterCB  = std::function<void(const RawDelta&)>;
    using PostFilterCB = std::function<void(const RawDelta&, const FilteredDelta&)>;

    // ── Construction ───────────────────────────────────────────────────
    MouseHookManager();
    ~MouseHookManager();

    // Non-copyable, non-movable
    MouseHookManager(const MouseHookManager&)            = delete;
    MouseHookManager& operator=(const MouseHookManager&) = delete;

    // ── Hook lifecycle ─────────────────────────────────────────────────
    // Returns false if SetWindowsHookEx fails (usually: not admin)
    bool install();
    void uninstall();
    bool isInstalled() const noexcept { return m_hHook != nullptr; }

    // ── Filter selection ───────────────────────────────────────────────
    void        setFilterType(FilterType type) noexcept;
    FilterType  getFilterType() const noexcept { return m_filterType; }

    // Direct access for GUI parameter binding
    OneEuroFilter2D& oneEuro() noexcept { return m_oneEuro2D; }
    EMAFilter2D&     ema()     noexcept { return m_ema2D; }
    KalmanFilter2D&  kalman()  noexcept { return m_kalman2D; }

    // ── Enable / disable (lock-free, hotkey-safe) ──────────────────────
    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    // ── Settings (atomic, safe to call from any thread) ────────────────
    void setDeadzone(int pixels)     noexcept { m_deadzone.store(pixels, std::memory_order_relaxed); }
    void setMaxDelta(int pixels)     noexcept { m_maxDelta.store(pixels, std::memory_order_relaxed); }
    int  getDeadzone() const         noexcept { return m_deadzone.load(std::memory_order_relaxed); }
    int  getMaxDelta() const         noexcept { return m_maxDelta.load(std::memory_order_relaxed); }

    // ── Callbacks for GUI real-time display ────────────────────────────
    void setPreFilterCB(PreFilterCB cb)   { m_preCB  = std::move(cb); }
    void setPostFilterCB(PostFilterCB cb) { m_postCB = std::move(cb); }

    // ── Statistics (for GUI) ───────────────────────────────────────────
    uint64_t totalEvents()       const noexcept { return m_eventCount.load(std::memory_order_relaxed); }
    uint64_t filteredEvents()    const noexcept { return m_filteredCount.load(std::memory_order_relaxed); }
    double   avgLatencyUs()      const noexcept { return m_avgLatencyUs.load(std::memory_order_relaxed); }

    // ── Heartbeat for hook health monitoring ───────────────────────────
    // Incremented on every WM_MOUSEMOVE event. If it stops incrementing,
    // the hook may have been uninstalled by another application.
    uint64_t heartbeat()         const noexcept { return m_heartbeat.load(std::memory_order_relaxed); }

private:
    // ── Static hook procedure (called by Windows) ──────────────────────
    static LRESULT CALLBACK lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    // ── Singleton pointer for hook proc → instance access ──────────────
    static MouseHookManager* s_instance;

    // ── Internal processing (called from hook proc) ────────────────────
    bool processMouseMove(MSLLHOOKSTRUCT* pMouse);

    // ── Inject filtered movement via SendInput ────────────────────────
    void injectMovement(double fdx, double fdy);

    // ── Windows hook handle ────────────────────────────────────────────
    HHOOK m_hHook = nullptr;

    // ── Filter pipeline ────────────────────────────────────────────────
    FilterType m_filterType = FilterType::OneEuro;
    OneEuroFilter2D m_oneEuro2D;
    EMAFilter2D     m_ema2D;
    KalmanFilter2D  m_kalman2D;

    // ── State (atomic — cross-thread access) ───────────────────────────
    std::atomic<bool> m_enabled{false};
    std::atomic<int>  m_deadzone{0};
    std::atomic<int>  m_maxDelta{100};

    // ── Statistics (atomic, monotonic counters) ────────────────────────
    std::atomic<uint64_t> m_eventCount{0};
    std::atomic<uint64_t> m_filteredCount{0};
    std::atomic<double>   m_avgLatencyUs{0.0};
    std::atomic<uint64_t> m_heartbeat{0};

    // ── Callbacks (set once at init, read-only during operation) ───────
    PreFilterCB  m_preCB;
    PostFilterCB m_postCB;

    // ── Timing (QPC) ───────────────────────────────────────────────────
    LARGE_INTEGER m_qpcFreq{};      // counts per second
    LARGE_INTEGER m_lastQPC{};      // previous hook invocation
    double        m_invFreq = 0.0;  // 1.0 / qpcFreq (precomputed)

    // ── Delta tracking (only accessed from hook thread — no atomics) ───
    LONG   m_lastRawX = 0;          // pt.x from previous hook call
    LONG   m_lastRawY = 0;          // pt.y from previous hook call
    double m_fractX   = 0.0;        // sub-pixel fractional accumulator X
    double m_fractY   = 0.0;        // sub-pixel fractional accumulator Y
    bool   m_posInitialized = false;// first-call flag for position init

    // ── Latency tracking (exponential moving average) ──────────────────
    double m_latencyEMA = 0.0;
};

} // namespace aether
