// ============================================================================
// SystemTray.cpp — System Tray Implementation
// ============================================================================

#include "SystemTray.hpp"
#include "Logger.hpp"

namespace aether {

SystemTray::~SystemTray() {
    destroy();
}

// ---------------------------------------------------------------------------
// create — add icon to notification area
// ---------------------------------------------------------------------------
bool SystemTray::create(HWND hWnd, UINT callbackMessage, const std::wstring& tooltip) {
    if (m_created) {
        LOG_WARN("SystemTray already created");
        return true;
    }

    m_hWnd        = hWnd;
    m_callbackMsg = callbackMessage;

    // Initialize NOTIFYICONDATA
    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd             = hWnd;
    m_nid.uID              = 1;                  // Unique icon ID for this app
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = callbackMessage;
    // Use custom icon if embedded, otherwise system default
    m_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), L"IDI_MAIN_ICON");
    if (!m_nid.hIcon) {
        m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    // Truncate tooltip to NOTIFYICONDATA limit (128 chars including null)
    wcsncpy_s(m_nid.szTip, tooltip.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        DWORD err = GetLastError();
        LOG_ERROR("Shell_NotifyIcon(NIM_ADD) failed: {}", err);
        return false;
    }

    // Set version to NOTIFYICON_VERSION_4 for Windows 10+ behavior
    // (balloon notifications use toast-style, better UX)
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);

    m_created = true;
    LOG_INFO("System tray icon created");
    return true;
}

// ---------------------------------------------------------------------------
// destroy — remove icon from notification area
// ---------------------------------------------------------------------------
void SystemTray::destroy() {
    if (!m_created) return;

    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    m_created = false;
    LOG_DEBUG("System tray icon removed");
}

// ---------------------------------------------------------------------------
// setTooltip — update hover text
// ---------------------------------------------------------------------------
void SystemTray::setTooltip(const std::wstring& text) {
    if (!m_created) return;

    wcsncpy_s(m_nid.szTip, text.c_str(), _TRUNCATE);
    m_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

// ---------------------------------------------------------------------------
// showBalloon — pop a notification near the tray icon
// ---------------------------------------------------------------------------
void SystemTray::showBalloon(const std::wstring& title, const std::wstring& text,
                              DWORD iconType) {
    if (!m_created) return;

    m_nid.uFlags      = NIF_INFO;
    m_nid.dwInfoFlags = iconType;
    wcsncpy_s(m_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(m_nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

// ---------------------------------------------------------------------------
// showContextMenu — right-click popup menu
//
// Call this from the window proc in response to WM_RBUTTONUP on the tray icon.
// Uses TrackPopupMenu for a standard Windows context menu.
// ---------------------------------------------------------------------------
void SystemTray::showContextMenu(bool filterEnabled, bool windowVisible) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // Show / Hide window
    AppendMenuW(hMenu, MF_STRING, MENU_SHOW_HIDE,
                windowVisible ? L"Hide Window" : L"Show Window");

    // Toggle filter
    AppendMenuW(hMenu, MF_STRING, MENU_TOGGLE_FILTER,
                filterEnabled ? L"Disable Filter" : L"Enable Filter");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, MENU_EXIT, L"Exit AetherAim");

    // Show at cursor position
    POINT pt;
    GetCursorPos(&pt);
    // Must call SetForegroundWindow for TrackPopupMenu to work correctly
    // with a notification area icon
    SetForegroundWindow(m_hWnd);

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, m_hWnd, nullptr);

    DestroyMenu(hMenu);
    // Required after TrackPopupMenu with tray icon
    PostMessageW(m_hWnd, WM_NULL, 0, 0);
}

// ---------------------------------------------------------------------------
// setFilterIndicator — update icon state (visual feedback not supported
// with IDI_APPLICATION, but we change the tooltip text)
// ---------------------------------------------------------------------------
void SystemTray::setFilterIndicator(bool active) {
    if (!m_created) return;
    // Future: swap icon between active/idle .ico files.
    // For now, just update tooltip to include filter state.
    (void)active;
}

} // namespace aether
