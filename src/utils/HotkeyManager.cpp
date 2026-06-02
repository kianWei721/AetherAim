// ============================================================================
// HotkeyManager.cpp — Implementation with Sleep/Wake Resilience
// ============================================================================

#include "HotkeyManager.hpp"
#include "Logger.hpp"
#include <chrono>
#include <algorithm>

namespace aether {

HotkeyManager::~HotkeyManager() {
    stop();
}

// ---------------------------------------------------------------------------
// configure
// ---------------------------------------------------------------------------
void HotkeyManager::configure(int vkCode, int vkCodeAlt, Callback cb) {
    m_vkCode    = vkCode;
    m_vkCodeAlt = vkCodeAlt;
    m_callback  = std::move(cb);
    LOG_DEBUG("HotkeyManager configured: primary=0x{:02X}, alt=0x{:02X}",
              m_vkCode, m_vkCodeAlt);
}

// ---------------------------------------------------------------------------
// start — launch polling thread
// ---------------------------------------------------------------------------
void HotkeyManager::start() {
    if (m_running.load(std::memory_order_acquire)) {
        LOG_WARN("HotkeyManager already running");
        return;
    }

    m_running.store(true, std::memory_order_release);
    m_lastTick = GetTickCount();
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    m_lastQPC = qpc.QuadPart;

    m_thread = std::thread(&HotkeyManager::pollLoop, this);
    LOG_INFO("HotkeyManager started");
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------
void HotkeyManager::stop() {
    if (!m_running.load(std::memory_order_acquire)) return;

    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    LOG_INFO("HotkeyManager stopped");
}

// ---------------------------------------------------------------------------
// detectSleepResume — check for large time gaps (system suspend)
//
// If the tick count or QPC has jumped by more than 5 seconds, the system
// likely just resumed from sleep. In that case, we reset the key state
// tracking to avoid a "stuck key" phantom toggle.
// ---------------------------------------------------------------------------
bool HotkeyManager::detectSleepResume() {
    DWORD curTick = GetTickCount();
    LARGE_INTEGER curQPC;
    QueryPerformanceCounter(&curQPC);

    // GetTickCount wraps every 49.7 days. Handle wraparound.
    DWORD tickDelta;
    if (curTick >= m_lastTick) {
        tickDelta = curTick - m_lastTick;
    } else {
        tickDelta = (0xFFFFFFFF - m_lastTick) + curTick + 1;
    }

    m_lastTick = curTick;
    m_lastQPC  = curQPC.QuadPart;

    // If more than 5 seconds elapsed between polls, treat as resume
    return tickDelta > 5000;
}

// ---------------------------------------------------------------------------
// pollLoop — main polling thread at ~200 Hz
// ---------------------------------------------------------------------------
void HotkeyManager::pollLoop() {
    using namespace std::chrono_literals;

    bool prevPrimary = false;
    bool prevAlt     = false;

    LOG_DEBUG("HotkeyManager poll loop started");

    while (m_running.load(std::memory_order_acquire)) {
        // ── Sleep/wake detection ────────────────────────────────────
        if (detectSleepResume()) {
            LOG_INFO("System resume detected — resetting hotkey state");
            prevPrimary = false;
            prevAlt     = false;
            // Re-read current key state to avoid phantom edge
        }

        // ── Poll GetAsyncKeyState ───────────────────────────────────
        // Bit 15 (0x8000): key is currently down
        // Bit 0  (0x0001): key was pressed since last call (we use edge detect)
        bool curPrimary = (GetAsyncKeyState(m_vkCode) & 0x8000) != 0;
        bool curAlt     = (GetAsyncKeyState(m_vkCodeAlt) & 0x8000) != 0;

        // ── Edge detection: rising edge = toggle ────────────────────
        if (curPrimary && !prevPrimary) {
            bool newState = !m_toggled.load(std::memory_order_acquire);
            m_toggled.store(newState, std::memory_order_release);
            if (m_callback) {
                m_callback(newState);
            }
            LOG_DEBUG("Hotkey toggle (primary): {}", newState ? "ON" : "OFF");
        }

        if (curAlt && !prevAlt) {
            bool newState = !m_toggled.load(std::memory_order_acquire);
            m_toggled.store(newState, std::memory_order_release);
            if (m_callback) {
                m_callback(newState);
            }
            LOG_DEBUG("Hotkey toggle (alt): {}", newState ? "ON" : "OFF");
        }

        prevPrimary = curPrimary;
        prevAlt     = curAlt;

        // ── Adaptive sleep ──────────────────────────────────────────
        // 5ms = 200 Hz. Fast enough for responsive hotkey detection,
        // slow enough to use <0.1% CPU on modern cores.
        std::this_thread::sleep_for(5ms);
    }

    LOG_DEBUG("HotkeyManager poll loop exited");
}

} // namespace aether
