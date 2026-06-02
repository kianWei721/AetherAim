#pragma once
// ============================================================================
// HotkeyManager.hpp — Global Hotkey Poller + Sleep/Wake Resilience
//
// Polls GetAsyncKeyState for the configured toggle key(s) at ~200 Hz.
// Detects and handles system suspend/resume to avoid stuck key states.
//
// Threading: runs a dedicated polling thread. Callback is invoked from
// the polling thread — callback implementors should use atomic stores
// or PostMessage to communicate with other threads.
// ============================================================================

#include <Windows.h>
#include <functional>
#include <thread>
#include <atomic>
#include <string>

namespace aether {

class HotkeyManager {
public:
    // Callback signature: void(bool toggled)
    // 'toggled' = true if filter was just turned ON, false if OFF.
    using Callback = std::function<void(bool toggled)>;

    HotkeyManager() = default;
    ~HotkeyManager();

    // Non-copyable
    HotkeyManager(const HotkeyManager&)            = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // ── Configuration ──────────────────────────────────────────────────
    // vkCode / vkCodeAlt: virtual key codes (VK_NUMLOCK, VK_F8, etc.)
    void configure(int vkCode, int vkCodeAlt, Callback cb);

    // Change hotkey at runtime
    void setHotkey(int vkCode)      { m_vkCode    = vkCode; }
    void setHotkeyAlt(int vkCode)   { m_vkCodeAlt = vkCode; }

    // ── Lifecycle ──────────────────────────────────────────────────────
    void start();
    void stop();
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    // ── State ──────────────────────────────────────────────────────────
    bool isToggled() const { return m_toggled.load(std::memory_order_acquire); }
    void setToggled(bool v) { m_toggled.store(v, std::memory_order_release); }

private:
    void pollLoop();

    // Detect system resume from sleep (tick count jumps)
    bool detectSleepResume();

    int      m_vkCode    = VK_NUMLOCK;
    int      m_vkCodeAlt = VK_F8;
    Callback m_callback;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_toggled{false};
    std::thread       m_thread;

    // Sleep/wake detection
    DWORD   m_lastTick   = 0;
    int64_t m_lastQPC    = 0;   // QPC for detecting large time gaps
};

} // namespace aether
