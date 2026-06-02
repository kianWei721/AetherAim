#pragma once
// ============================================================================
// SystemTray.hpp — Windows Notification Area (System Tray) Manager
//
// Creates a tray icon with context menu. Integrates with the existing
// GUI window via a user-defined callback message (WM_APP + N).
//
// Usage:
//   1. SystemTray tray;
//   2. tray.create(hWnd, WM_APP + 1, L"AetherAim");
//   3. In WndProc, handle WM_APP+1 to respond to tray events.
//   4. tray.destroy() on shutdown.
//
// Context menu IDs (received as wParam in the callback message):
//   MENU_TOGGLE_FILTER  — toggle filter on/off
//   MENU_SHOW_HIDE      — show/hide main window
//   MENU_EXIT           — quit application
// ============================================================================

#include <Windows.h>
#include <string>

namespace aether {

class SystemTray {
public:
    // Context menu command IDs (passed as wParam to callback)
    static constexpr UINT MENU_TOGGLE_FILTER = 100;
    static constexpr UINT MENU_SHOW_HIDE     = 101;
    static constexpr UINT MENU_EXIT          = 102;

    SystemTray() = default;
    ~SystemTray();

    // Non-copyable
    SystemTray(const SystemTray&)            = delete;
    SystemTray& operator=(const SystemTray&) = delete;

    // Create tray icon. hWnd receives callbackMessage on icon interaction.
    // tooltip: hover text. Uses IDI_APPLICATION as default icon.
    bool create(HWND hWnd, UINT callbackMessage, const std::wstring& tooltip);

    // Remove tray icon
    void destroy();

    bool isCreated() const { return m_created; }

    // Update tooltip text dynamically
    void setTooltip(const std::wstring& text);

    // Show a balloon notification (Windows 10+ toast-style)
    void showBalloon(const std::wstring& title, const std::wstring& text,
                     DWORD iconType = NIIF_INFO);

    // Show context menu at current cursor position
    void showContextMenu(bool filterEnabled, bool windowVisible);

    // Toggle icon to reflect filter state
    void setFilterIndicator(bool active);

private:
    NOTIFYICONDATAW m_nid{};
    bool            m_created = false;
    HWND            m_hWnd    = nullptr;
    UINT            m_callbackMsg = 0;
};

} // namespace aether
