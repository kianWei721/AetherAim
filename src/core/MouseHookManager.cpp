// ============================================================================
// MouseHookManager.cpp — WH_MOUSE_LL Hook Implementation
//
// DATA FLOW (per mouse event):
//
//   Hardware Mouse
//        │  raw relative delta from HID driver
//        ▼
//   Windows Input System
//        │  converts to absolute screen coords → MSLLHOOKSTRUCT.pt
//        ▼
//   lowLevelMouseProc()         ← WH_MOUSE_LL callback
//        │
//        ├─ [injected?] ────────► pass through (CallNextHookEx)
//        │
//        ├─ [!enabled] ─────────► pass through, reset tracking
//        │
//        ├─ [!posInitialized] ──► bootstrap tracking, pass through
//        │
//        ▼
//   processMouseMove()
//        │
//        ├─ Compute raw delta:  dx = pt.x - m_lastRawX
//        ├─ Compute dt via QPC:  dt = (now - lastQPC) / freq
//        ├─ Deadzone check:     if |dx|,|dy| < deadzone → suppress, no injection
//        ├─ MaxDelta clamp:     clamp(dx/dy, ±maxDelta)
//        │
//        ├─ Filter dispatch:
//        │    OneEuro → m_oneEuro2D.filter(dx, dy, dt)
//        │    EMA     → m_ema2D.filter(dx, dy)
//        │    Kalman  → m_kalman2D.filter(dx, dy)
//        │    None    → passthrough (dx, dy)
//        │
//        ├─ Sub-pixel accumulation → integer part → injectMovement()
//        │
//        ├─ Update statistics (event count, latency EMA)
//        ├─ Invoke post-filter callback (GUI plot data)
//        │
//        ▼
//   Return 1 (suppress original event)
//
// LATENCY BUDGET (per event, target < 5ms @ 1000Hz = 5μs avg):
//   QPC read:              ~0.02 μs (RDTSC on modern CPUs)
//   Delta + deadzone:      ~0.01 μs
//   OneEuroFilter::filter: ~0.05 μs (2x 1D filter, L1 cache hot)
//   SendInput call:        ~2-5  μs (syscall overhead, dominates)
//   Total per event:       ~3-5  μs  ✓ well within budget
// ============================================================================

#include "MouseHookManager.hpp"
#include "utils/Logger.hpp"
#include <algorithm>
#include <cmath>

namespace aether {

// ---------------------------------------------------------------------------
// Static singleton pointer — accessed ONLY by hook proc to reach instance
// ---------------------------------------------------------------------------
MouseHookManager* MouseHookManager::s_instance = nullptr;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------
MouseHookManager::MouseHookManager() {
    QueryPerformanceFrequency(&m_qpcFreq);
    m_invFreq = 1.0 / static_cast<double>(m_qpcFreq.QuadPart);
    QueryPerformanceCounter(&m_lastQPC);
}

MouseHookManager::~MouseHookManager() {
    uninstall();
}

// ---------------------------------------------------------------------------
// install — SetWindowsHookEx for WH_MOUSE_LL
//
// PREREQUISITES:
//   - Process must run as Administrator (Windows 10+ enforces this for
//     global hooks with UIPI — User Interface Privilege Isolation).
//   - The installing thread MUST pump messages (handled by GUIApp::run).
//
// Returns false if hook installation fails (GetLastError for details).
// ---------------------------------------------------------------------------
bool MouseHookManager::install() {
    if (m_hHook) {
        LOG_WARN("Hook already installed");
        return true;
    }

    s_instance = this;

    // Reset position tracking
    m_posInitialized = false;

    // WH_MOUSE_LL with dwThreadId=0 → global hook (all threads/processes)
    // The hook proc executes on THIS thread (the installer), which must
    // have a message pump — our GUIApp::run() PeekMessage loop provides this.
    m_hHook = SetWindowsHookExW(
        WH_MOUSE_LL,
        lowLevelMouseProc,
        GetModuleHandleW(nullptr),  // hMod: this module
        0                           // dwThreadId: 0 = global
    );

    if (!m_hHook) {
        DWORD err = GetLastError();
        LOG_ERROR("SetWindowsHookEx(WH_MOUSE_LL) failed — error code: {}", err);
        s_instance = nullptr;

        // Provide actionable error messages for common failure codes
        switch (err) {
        case 5:   // ERROR_ACCESS_DENIED
            LOG_ERROR("→ Access denied. Are you running as Administrator?");
            break;
        case 8:   // ERROR_NOT_ENOUGH_MEMORY
            LOG_ERROR("→ Not enough memory for hook.");
            break;
        case 1450: // ERROR_NO_SYSTEM_RESOURCES
            LOG_ERROR("→ System resources exhausted. Too many hooks?");
            break;
        }
        return false;
    }

    LOG_INFO("WH_MOUSE_LL hook installed successfully (handle=0x{:X})",
             reinterpret_cast<uintptr_t>(m_hHook));
    return true;
}

// ---------------------------------------------------------------------------
// uninstall
// ---------------------------------------------------------------------------
void MouseHookManager::uninstall() {
    if (m_hHook) {
        UnhookWindowsHookEx(m_hHook);
        m_hHook    = nullptr;
        s_instance = nullptr;
        m_posInitialized = false;
        LOG_INFO("WH_MOUSE_LL hook uninstalled");
    }
}

// ---------------------------------------------------------------------------
// setFilterType — switch filter backend, reset all state
// ---------------------------------------------------------------------------
void MouseHookManager::setFilterType(FilterType type) noexcept {
    m_filterType = type;
    m_oneEuro2D.reset();
    m_ema2D.reset();
    m_kalman2D.reset();
    m_posInitialized = false;  // force position re-bootstrap
    LOG_DEBUG("FilterType set to {}",
              type == FilterType::OneEuro ? "OneEuro" :
              type == FilterType::EMA     ? "EMA" :
              type == FilterType::Kalman  ? "Kalman" : "None");
}

// ---------------------------------------------------------------------------
// setEnabled — lock-free toggle (safe from hotkey thread or GUI)
// ---------------------------------------------------------------------------
void MouseHookManager::setEnabled(bool on) noexcept {
    bool wasEnabled = m_enabled.exchange(on, std::memory_order_release);
    if (on && !wasEnabled) {
        // Just enabled — reset filter state for clean start
        m_oneEuro2D.reset();
        m_ema2D.reset();
        m_kalman2D.reset();
        m_posInitialized = false;
    }
    // If disabling, no action needed — hook proc will pass through
}

bool MouseHookManager::isEnabled() const noexcept {
    return m_enabled.load(std::memory_order_acquire);
}

// ============================================================================
// lowLevelMouseProc — Static hook callback (called by Windows on THIS thread)
//
// CRITICAL: This function runs at elevated IRQL for input processing.
// DO NOT: allocate memory, take locks, perform I/O, call blocking APIs.
// DO:     read atomics, compute math, call SendInput, update counters.
//
// Return value:
//   < 0  → pass to CallNextHookEx (required for nCode < 0)
//   = 0  → pass to CallNextHookEx (system processes the event normally)
//   > 0  → SUPPRESS — system does NOT process this event
// ============================================================================
LRESULT CALLBACK MouseHookManager::lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // ── Must call CallNextHookEx for nCode < 0 ──
    if (nCode < 0 || !s_instance) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // ── Only process WM_MOUSEMOVE ──
    if (wParam != WM_MOUSEMOVE) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    auto* self = s_instance;
    auto* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

    // ── Detect injected events (our own SendInput) — pass through ──
    // LLMHF_INJECTED = 0x00000001: event came from SendInput / mouse_event
    // Without this check, our injected movements would re-enter the filter,
    // causing an infinite recursion or double-filtering.
    if (pMouse->flags & LLMHF_INJECTED) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // ── Increment event counter + heartbeat ──
    self->m_eventCount.fetch_add(1, std::memory_order_relaxed);
    self->m_heartbeat.store(self->m_eventCount.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);

    // ── Check if filtering is enabled ──
    if (!self->m_enabled.load(std::memory_order_acquire)) {
        // Disabled: just pass through. Reset position tracking so next
        // enable bootstraps cleanly.
        self->m_posInitialized = false;
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // ── Process this mouse event ──
    bool suppressed = self->processMouseMove(pMouse);

    if (suppressed) {
        self->m_filteredCount.fetch_add(1, std::memory_order_relaxed);
        return 1;  // Suppress: system does NOT move cursor
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ============================================================================
// processMouseMove — extract delta, filter, inject
//
// Returns true if the original event was suppressed (we injected our own).
// ============================================================================
bool MouseHookManager::processMouseMove(MSLLHOOKSTRUCT* pMouse) {
    // ── Timing: compute dt since last event ──
    LARGE_INTEGER nowQPC;
    QueryPerformanceCounter(&nowQPC);
    double dt = static_cast<double>(nowQPC.QuadPart - m_lastQPC.QuadPart) * m_invFreq;
    m_lastQPC = nowQPC;

    // Guard against implausible dt (first call, system resume from sleep, etc.)
    // Cap at 500ms — anything longer means the system was suspended/idle.
    if (dt > 0.5) {
        dt = 0.016;  // assume ~60Hz baseline
        m_posInitialized = false;  // force re-bootstrap
    }
    if (dt < 1e-6) {
        dt = 1e-6;  // minimum 1μs to avoid division by zero
    }

    // ── Position bootstrap ──
    // On first event after enable (or after dt timeout), capture the
    // raw position and let the event pass through. This avoids a
    // discontinuity in delta computation.
    if (!m_posInitialized) {
        m_lastRawX = pMouse->pt.x;
        m_lastRawY = pMouse->pt.y;
        m_posInitialized = true;
        m_fractX = 0.0;
        m_fractY = 0.0;
        return false;  // pass through — bootstrap event, no filtering
    }

    // ── Compute raw delta ──
    double dx = static_cast<double>(pMouse->pt.x - m_lastRawX);
    double dy = static_cast<double>(pMouse->pt.y - m_lastRawY);

    // Update raw tracking BEFORE deadzone check — even if we suppress this
    // event, we need to track the accumulated position for the next delta.
    m_lastRawX = pMouse->pt.x;
    m_lastRawY = pMouse->pt.y;

    // ── Handle monitor transitions / coordinate overflow ──
    // A delta > 10000 px likely means the cursor jumped to a different
    // monitor or the system reported bogus coordinates. Clamp aggressively.
    if (std::abs(dx) > 10000.0 || std::abs(dy) > 10000.0) {
        LOG_DEBUG("Large delta detected ({:.0f}, {:.0f}) — clamping", dx, dy);
        dx = std::clamp(dx, -10000.0, 10000.0);
        dy = std::clamp(dy, -10000.0, 10000.0);
    }

    // ── Pre-filter callback (for GUI monitoring) ──
    if (m_preCB) {
        RawDelta raw{};
        raw.dx        = static_cast<LONG>(dx);
        raw.dy        = static_cast<LONG>(dy);
        raw.timestamp  = pMouse->time;
        raw.dt         = dt;
        m_preCB(raw);
    }

    // ── Deadzone ──
    int dz = m_deadzone.load(std::memory_order_relaxed);
    if (dz > 0 && std::abs(dx) < dz && std::abs(dy) < dz) {
        // Suppress micro-movements entirely — don't even inject.
        // The cursor stays where it is. Next raw delta will be relative
        // to current pt (which we already stored in m_lastRaw).
        return true;
    }

    // ── Max-delta clamp ──
    int maxD = m_maxDelta.load(std::memory_order_relaxed);
    if (maxD > 0) {
        dx = std::clamp(dx, -static_cast<double>(maxD), static_cast<double>(maxD));
        dy = std::clamp(dy, -static_cast<double>(maxD), static_cast<double>(maxD));
    }

    // ── Filter dispatch ────────────────────────────────────────────
    double fdx, fdy;

    switch (m_filterType) {
    case FilterType::OneEuro: {
        auto result = m_oneEuro2D.filter(dx, dy, dt);
        fdx = result.x;
        fdy = result.y;
        break;
    }
    case FilterType::EMA: {
        auto result = m_ema2D.filter(dx, dy);
        fdx = result.x;
        fdy = result.y;
        break;
    }
    case FilterType::Kalman: {
        auto result = m_kalman2D.filter(dx, dy);
        fdx = result.x;
        fdy = result.y;
        break;
    }
    case FilterType::None:
    default:
        fdx = dx;
        fdy = dy;
        break;
    }

    // ── Sub-pixel accumulation ──
    // Without this, slow mouse movements (e.g. 1 px/event filtered to
    // 0.3 px/event) would round to 0 every time → visible stiction.
    // The fractional accumulator preserves the remainder across events.
    double totalX = fdx + m_fractX;
    double totalY = fdy + m_fractY;

    LONG intX = static_cast<LONG>(totalX);
    LONG intY = static_cast<LONG>(totalY);

    // Clamp fractional accumulators to prevent unbounded growth from
    // numerical edge cases (e.g. repeated sub-pixel rounding in one direction).
    // Range [-1, 1] is sufficient since SendInput only consumes integer px.
    m_fractX = std::clamp(totalX - static_cast<double>(intX), -1.0, 1.0);
    m_fractY = std::clamp(totalY - static_cast<double>(intY), -1.0, 1.0);

    if (intX != 0 || intY != 0) {
        injectMovement(intX, intY);
    }

    // ── Post-filter callback ──
    if (m_postCB) {
        RawDelta raw{};
        raw.dx       = static_cast<LONG>(dx);
        raw.dy       = static_cast<LONG>(dy);
        raw.timestamp = pMouse->time;
        raw.dt        = dt;

        FilteredDelta filt{};
        filt.fx        = fdx;
        filt.fy        = fdy;
        filt.rawSpeed  = std::sqrt(dx * dx + dy * dy) / dt;
        filt.filtSpeed = std::sqrt(fdx * fdx + fdy * fdy) / dt;
        filt.dt        = dt;

        m_postCB(raw, filt);
    }

    // ── Update latency tracking (exponential moving average, μs) ──
    LARGE_INTEGER afterQPC;
    QueryPerformanceCounter(&afterQPC);
    double latencyUs = static_cast<double>(afterQPC.QuadPart - nowQPC.QuadPart)
                       * m_invFreq * 1'000'000.0;

    constexpr double latencyAlpha = 0.05;  // slow EMA for stable display
    m_latencyEMA = latencyAlpha * latencyUs + (1.0 - latencyAlpha) * m_latencyEMA;
    m_avgLatencyUs.store(m_latencyEMA, std::memory_order_relaxed);

    return true;  // suppressed — we injected our own movement
}

// ---------------------------------------------------------------------------
// injectMovement — send filtered delta via SendInput
//
// Uses MOUSEEVENTF_MOVE (relative mode). The injected event will re-enter
// our hook proc with LLMHF_INJECTED flag set → passed through to the system.
//
// NOTE: MOUSEEVENTF_MOVE without MOUSEEVENTF_ABSOLUTE means dx/dy are
// relative pixel counts. Windows applies its own mouse speed/acceleration
// settings to these values, which could distort our carefully filtered
// output for users with non-default pointer speed.
//
// For FPS games this is typically NOT an issue because:
//   a) Competitive players use 6/11 Windows sensitivity (1:1, no accel)
//   b) CS2/Valorant/OW2 use raw input (bypass Windows cursor entirely)
//
// For a future revision, consider using MOUSEEVENTF_ABSOLUTE with
// virtual coordinate tracking to bypass Windows acceleration.
// ---------------------------------------------------------------------------
void MouseHookManager::injectMovement(double fdx, double fdy) {
    // SendInput with absolute movement to bypass Windows pointer
    // acceleration. Track virtual screen coordinates.
    // For now, using relative movement — see note above.

    INPUT input{};
    input.type       = INPUT_MOUSE;
    input.mi.dx      = static_cast<LONG>(fdx);
    input.mi.dy      = static_cast<LONG>(fdy);
    input.mi.dwFlags = MOUSEEVENTF_MOVE;

    // MOUSEEVENTF_MOVE_NOCOALESCE (Win8+) prevents the system from
    // merging consecutive mouse movements — important for precision.
    // Not strictly necessary since our filter already smooths, but
    // adds determinism.
    input.mi.dwFlags |= MOUSEEVENTF_MOVE_NOCOALESCE;

    SendInput(1, &input, sizeof(INPUT));
}

} // namespace aether
