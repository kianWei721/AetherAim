#pragma once
// ============================================================================
// GUIApp.hpp — Dear ImGui DX11 Overlay Application
//
// Creates a standard window with:
//   - Status bar (filter state, profile, latency)
//   - Monitor tab: real-time raw-vs-filtered plot (implot) + stats
//   - Profile tab: radar chart + manual profile editor
//   - Settings tab: filter type switch, parameter sliders, advanced options
//
// System tray integration:
//   - Minimize to tray on window close
//   - Right-click tray icon → context menu (Show/Hide, Toggle, Exit)
//   - Auto-save profile on a timer (configurable interval)
//
// Thread Model:
//   GUIApp::run() runs on the main thread, which ALSO pumps the
//   WH_MOUSE_LL hook via PeekMessage. Therefore, hook callbacks and
//   GUI rendering are serialized on the same thread — no locks needed
//   for the plot data ring buffer.
// ============================================================================

#include <Windows.h>
#include <d3d11.h>
#include <string>
#include <array>
#include <vector>
#include <atomic>
#include <deque>
#include <chrono>

// Forward declarations
struct ImGuiContext;
struct ImPlotContext;

namespace aether {

class ConfigManager;
class ProfileManager;
class InputProcessor;
class SystemTray;
class GameInjector;
class CalibrationWizard;

// ============================================================================
// PlotPoint — single data point for the real-time scroll plot
// ============================================================================
struct alignas(16) PlotPoint {
    double time;
    float  rawX, rawY;
    float  filtX, filtY;
};

// ============================================================================
// Lock-free SPSC ring buffer for plot data
// ============================================================================
class PlotRingBuffer {
public:
    static constexpr int CAPACITY = 2048;

    PlotRingBuffer() { m_data.resize(CAPACITY); }

    void push(const PlotPoint& pt) {
        int w = m_writeIdx.load(std::memory_order_relaxed);
        m_data[w % CAPACITY] = pt;
        m_writeIdx.store(w + 1, std::memory_order_release);
    }

    std::vector<PlotPoint> drain() {
        int w = m_writeIdx.load(std::memory_order_acquire);
        std::vector<PlotPoint> result;
        result.reserve(w - m_readIdx);
        while (m_readIdx < w) {
            result.push_back(m_data[m_readIdx % CAPACITY]);
            ++m_readIdx;
        }
        return result;
    }

    void reset() {
        m_writeIdx.store(0, std::memory_order_relaxed);
        m_readIdx = 0;
    }

private:
    std::vector<PlotPoint> m_data;
    std::atomic<int>       m_writeIdx{0};
    int                    m_readIdx = 0;
};

// ============================================================================
// GUIApp
// ============================================================================
class GUIApp {
public:
    // ── Tray callback message ID (must match SystemTray usage) ─────────
    static constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;

    GUIApp();
    ~GUIApp();

    // Initialize DX11 device, window, ImGui + ImPlot context
    bool init(ConfigManager* cfg, ProfileManager* profiles, InputProcessor* input,
              SystemTray* tray = nullptr, GameInjector* injector = nullptr);

    // Main loop — blocks until window closed (or requestShutdown)
    void run();

    // Signal shutdown from hotkey or tray menu
    void requestShutdown();

    // ── Window visibility ──────────────────────────────────────────────
    void hideWindow();
    void showWindow();
    void toggleVisibility();
    bool isVisible() const;
    void minimizeToTray();

    // ── Tray message handler (called from wndProc) ────────────────────
    // Returns true if the message was handled
    bool handleTrayMessage(WPARAM wParam, LPARAM lParam);

private:
    // ── Window / D3D11 setup ───────────────────────────────────────────
    bool createWindow();
    bool createDevice();
    void cleanup();

    // ── Render hierarchy ───────────────────────────────────────────────
    void render();
    void showStatusBar();
    void showMonitorTab();
    void showProfileTab();
    void showSettingsTab();
    void showAboutTab();
    void showGamesTab();

    // ── Periodic tasks ─────────────────────────────────────────────────
    void checkAutoSave();
    void checkConfigReload();
    void checkHookHealth();

    // ── Plot helpers ───────────────────────────────────────────────────
    void updatePlotData();
    void renderRealtimePlot();
    void renderStatsPanel();

    // ── Settings sync ──────────────────────────────────────────────────
    void pushSettingsToPipeline();
    void pullSettingsFromConfig();

    // ── Profile sync ───────────────────────────────────────────────────
    void pushProfileToPipeline();

    // ── Win32 window proc ──────────────────────────────────────────────
    static LRESULT CALLBACK wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ── D3D11 resources ────────────────────────────────────────────────
    ID3D11Device*           m_d3dDevice     = nullptr;
    ID3D11DeviceContext*    m_d3dContext    = nullptr;
    IDXGISwapChain*         m_swapChain     = nullptr;
    ID3D11RenderTargetView* m_renderTarget  = nullptr;
    HWND                    m_hWnd          = nullptr;
    WNDCLASSEXW             m_wc            = {};

    ImGuiContext*  m_imguiCtx  = nullptr;
    ImPlotContext* m_implotCtx = nullptr;

    // ── External references (not owned) ────────────────────────────────
    ConfigManager*   m_config   = nullptr;
    ProfileManager*  m_profiles = nullptr;
    InputProcessor*  m_input    = nullptr;
    SystemTray*         m_tray     = nullptr;
    GameInjector*       m_injector = nullptr;
    CalibrationWizard*  m_calibration = nullptr;  // Owned; created on first use

    // ── State ──────────────────────────────────────────────────────────
    bool m_running        = false;
    bool m_minimizedToTray = false;
    bool m_showImGuiDemo  = false;
    bool m_showImPlotDemo = false;
    int  m_activeTab      = 0;

    // ── Auto-save ──────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point m_lastAutoSave;

    // ── Plot data ──────────────────────────────────────────────────────
    PlotRingBuffer m_ringBuffer;
    std::deque<PlotPoint> m_displayBuf;
    static constexpr size_t MAX_DISPLAY_PTS = 3000;
    double m_plotTimeMin = 0.0;
    double m_plotTimeMax = 3.0;
    double m_sessionStartTime = 0.0;
    bool   m_autoScroll = true;

    // ── Settings editor state ──────────────────────────────────────────
    int   m_editFilterType = 0;
    float m_editMinCutoff  = 1.2f;
    float m_editBeta       = 0.0f;
    float m_editDCutoff    = 1.0f;
    float m_editSpeedCoeff = 1.0f;
    float m_editEMAlpha    = 0.6f;
    float m_editKQ         = 0.01f;
    float m_editKR         = 0.1f;
    int   m_editDeadzone   = 0;
    int   m_editMaxDelta   = 100;
    int   m_editHotkey     = 0x90;
    int   m_editHotkeyAlt  = 0x77;

    // ── Profile editor state ───────────────────────────────────────────
    char  m_profileNameBuf[64]   = {};
    char  m_profileGameBuf[32]   = {};
    float m_editTremorSev        = 0.0f;
    float m_editOvershoot        = 0.0f;
    float m_editFatigue          = 0.0f;
    float m_editReactionDelay    = 0.0f;
    float m_editTremorSevX       = 0.0f;
    float m_editTremorSevY       = 0.0f;
    int   m_editDPI              = 800;
    float m_editSensitivity      = 1.0f;
    int   m_editPollingHz        = 1000;
    bool  m_profileDirty         = false;

    // ── Profile list ───────────────────────────────────────────────────
    std::vector<std::string> m_profileList;
    int    m_selectedProfileIdx = -1;
    bool   m_profileListDirty   = true;

    // ── Game injection state ───────────────────────────────────────────
    float  m_gameScanTimer = 0.0f;
    static constexpr float GAME_SCAN_INTERVAL = 2.0f;  // scan every 2s
};

} // namespace aether
