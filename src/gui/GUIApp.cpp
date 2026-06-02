// ============================================================================
// GUIApp.cpp — Dear ImGui DX11 Application Implementation
// ============================================================================

#include "GUIApp.hpp"
#include "config/ConfigManager.hpp"
#include "config/ProfileData.hpp"
#include "config/ProfileManager.hpp"
#include "core/InputProcessor.hpp"
#include "core/MouseHookManager.hpp"
#include "gui/Widgets.hpp"
#include "utils/Logger.hpp"
#include "utils/MathUtils.hpp"
#include "utils/SystemTray.hpp"
#include "injector/GameInjector.hpp"
#include "gui/CalibrationWizard.hpp"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>

namespace aether {

// Forward-declared by imgui_impl_win32.h
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// Construction / Destruction
// ============================================================================
GUIApp::GUIApp()  = default;
GUIApp::~GUIApp() = default;

// ============================================================================
// init — Create window, D3D11 device, ImGui context
// ============================================================================
bool GUIApp::init(ConfigManager* cfg, ProfileManager* profiles, InputProcessor* input,
                  SystemTray* tray, GameInjector* injector) {
    m_config   = cfg;
    m_profiles = profiles;
    m_input    = input;
    m_tray     = tray;
    m_injector = injector;

    if (!createWindow()) {
        LOG_ERROR("Failed to create GUI window");
        return false;
    }
    if (!createDevice()) {
        LOG_ERROR("Failed to create D3D11 device");
        return false;
    }

    // ── Dear ImGui context ───────────────────────────────────────────
    IMGUI_CHECKVERSION();
    m_imguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_imguiCtx);
    m_implotCtx = ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;          // Don't auto-save .ini
    io.LogFilename  = nullptr;

    // Styling: dark, slightly compact
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.FramePadding      = ImVec2(6, 3);
    style.ItemSpacing       = ImVec2(8, 4);
    style.ScrollbarSize     = 12.0f;

    // ImPlot styling
    ImPlot::StyleColorsDark();

    // Platform backends
    ImGui_ImplWin32_Init(m_hWnd);
    ImGui_ImplDX11_Init(m_d3dDevice, m_d3dContext);

    // ── Initialize settings editor from config ───────────────────────
    pullSettingsFromConfig();

    // ── Init auto-save timer ──────────────────────────────────────────
    m_lastAutoSave = std::chrono::steady_clock::now();

    // ── Register hook callback for plot data ─────────────────────────
    m_sessionStartTime = math::now_seconds();

    // Post-filter callback: feeds plot ring buffer AND calibration wizard
    m_input->hookManager().setPostFilterCB(
        [this](const RawDelta& raw, const FilteredDelta& filt) {
            // Plot data
            PlotPoint pt;
            pt.time  = math::now_seconds() - m_sessionStartTime;
            pt.rawX  = static_cast<float>(raw.dx);
            pt.rawY  = static_cast<float>(raw.dy);
            pt.filtX = static_cast<float>(filt.fx);
            pt.filtY = static_cast<float>(filt.fy);
            m_ringBuffer.push(pt);

            // Calibration wizard raw data feed
            if (m_calibration && m_calibration->isRunning()) {
                m_calibration->pushMouseDelta(
                    static_cast<double>(raw.dx),
                    static_cast<double>(raw.dy),
                    raw.dt);
            }
        }
    );

    LOG_INFO("GUI initialized successfully");
    return true;
}

// ============================================================================
// run — Main message loop with rendering
// ============================================================================
void GUIApp::run() {
    m_running = true;

    MSG msg{};
    while (m_running) {
        // Process all pending Windows messages (including WH_MOUSE_LL hook)
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                m_running = false;
            }
        }
        if (!m_running) break;

        render();
    }

    cleanup();
}

void GUIApp::requestShutdown() {
    m_running = false;
}

// ============================================================================
// Window & D3D11 creation
// ============================================================================
bool GUIApp::createWindow() {
    m_wc.cbSize        = sizeof(WNDCLASSEXW);
    m_wc.style         = CS_HREDRAW | CS_VREDRAW;
    m_wc.lpfnWndProc   = wndProc;
    m_wc.hInstance     = GetModuleHandleW(nullptr);
    m_wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    m_wc.hbrBackground = nullptr;   // D3D11 handles clearing
    m_wc.hIcon         = LoadIconW(m_wc.hInstance, L"IDI_MAIN_ICON");
    if (!m_wc.hIcon) m_wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    m_wc.hIconSm       = m_wc.hIcon;
    m_wc.lpszClassName = L"AetherAimGUI";

    RegisterClassExW(&m_wc);

    const auto& guiCfg = m_config->config().gui;

    m_hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        m_wc.lpszClassName,
        L"AetherAim — FPS Accessibility Assistant",
        WS_OVERLAPPEDWINDOW,
        guiCfg.windowX, guiCfg.windowY,
        guiCfg.windowW, guiCfg.windowH,
        nullptr, nullptr, m_wc.hInstance, this
    );

    if (!m_hWnd) return false;

    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);
    return true;
}

bool GUIApp::createDevice() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width  = 0;   // auto from window
    sd.BufferDesc.Height = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hWnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed   = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, &featureLevel, 1, D3D11_SDK_VERSION,
        &sd, &m_swapChain, &m_d3dDevice, nullptr, &m_d3dContext
    );
    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDeviceAndSwapChain failed: 0x{:08X}",
                  static_cast<uint32_t>(hr));
        return false;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                           reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        m_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &m_renderTarget);
        backBuffer->Release();
    }

    return true;
}

void GUIApp::cleanup() {
    // Save window position for next launch
    if (m_hWnd && m_config) {
        RECT rect;
        if (GetWindowRect(m_hWnd, &rect)) {
            auto& gui = m_config->configMutable().gui;
            gui.windowX = rect.left;
            gui.windowY = rect.top;
            gui.windowW = rect.right - rect.left;
            gui.windowH = rect.bottom - rect.top;
        }
    }

    if (m_calibration) {
        delete m_calibration;
        m_calibration = nullptr;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    if (m_renderTarget) m_renderTarget->Release();
    if (m_swapChain)    m_swapChain->Release();
    if (m_d3dContext)   m_d3dContext->Release();
    if (m_d3dDevice)    m_d3dDevice->Release();
    if (m_hWnd)         DestroyWindow(m_hWnd);
    UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
}

// ============================================================================
// render — Main frame dispatch
// ============================================================================
void GUIApp::render() {
    // Periodic maintenance
    checkAutoSave();
    checkConfigReload();
    checkHookHealth();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Full-window docking panel
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("AetherAim", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    showStatusBar();

    ImGui::PopStyleVar(2);

    if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Monitor")) {
            m_activeTab = 0;
            showMonitorTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Profile")) {
            m_activeTab = 1;
            showProfileTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            m_activeTab = 2;
            showSettingsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Games")) {
            m_activeTab = 4;
            showGamesTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            m_activeTab = 3;
            showAboutTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();

    // Dev tools
    if (m_showImGuiDemo)  ImGui::ShowDemoWindow(&m_showImGuiDemo);
    if (m_showImPlotDemo) ImPlot::ShowDemoWindow(&m_showImPlotDemo);

    // Render
    ImGui::Render();
    const float clearColor[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
    m_d3dContext->OMSetRenderTargets(1, &m_renderTarget, nullptr);
    m_d3dContext->ClearRenderTargetView(m_renderTarget, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_swapChain->Present(1, 0);
}

// ============================================================================
// showStatusBar — Top bar: filter state, profile, stats
// ============================================================================
void GUIApp::showStatusBar() {
    bool enabled = m_input->isEnabled();
    const auto& profile = m_profiles->active();

    // Filter state indicator
    ImGui::TextColored(
        enabled ? ImVec4(0.25f, 0.95f, 0.35f, 1.0f) : ImVec4(0.95f, 0.30f, 0.30f, 1.0f),
        enabled ? "● FILTER ACTIVE" : "○ FILTER PAUSED");

    ImGui::SameLine(160);
    ImGui::Text("| Profile: %s", profile.name.c_str());

    ImGui::SameLine(400);
    ImGui::Text("| Filter: %s",
        m_input->getFilterType() == FilterType::OneEuro ? "OneEuro" :
        m_input->getFilterType() == FilterType::EMA     ? "EMA" :
        m_input->getFilterType() == FilterType::Kalman  ? "Kalman" : "None");

    // Events / Latency on the right
    auto& hm = m_input->hookManager();
    ImGui::SameLine(ImGui::GetWindowWidth() - 340);
    ImGui::Text("Events: %llu | Latency:", hm.totalEvents());
    ImGui::SameLine();
    widgets::LatencyDisplay(hm.avgLatencyUs());

    ImGui::Separator();
}

// ============================================================================
// Monitor Tab — Real-time plot + stats panel
// ============================================================================
void GUIApp::showMonitorTab() {
    updatePlotData();

    // ── Left: plot (takes 70% width) ─────────────────────────────────
    float plotWidth = ImGui::GetContentRegionAvail().x * 0.68f;
    ImGui::BeginChild("PlotPanel", ImVec2(plotWidth, 0), ImGuiChildFlags_Border);
    renderRealtimePlot();
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right: stats panel ───────────────────────────────────────────
    ImGui::BeginChild("StatsPanel", ImVec2(0, 0), ImGuiChildFlags_Border);
    renderStatsPanel();
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// updatePlotData — drain ring buffer into display deque
// ---------------------------------------------------------------------------
void GUIApp::updatePlotData() {
    auto newPts = m_ringBuffer.drain();
    for (auto& pt : newPts) {
        m_displayBuf.push_back(pt);
    }
    // Trim old data
    while (m_displayBuf.size() > MAX_DISPLAY_PTS) {
        m_displayBuf.pop_front();
    }
}

// ---------------------------------------------------------------------------
// renderRealtimePlot — implot dual-axis scroll plot
// ---------------------------------------------------------------------------
void GUIApp::renderRealtimePlot() {
    if (m_displayBuf.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "Waiting for mouse events... Move your mouse.");
        return;
    }

    // Build time/value arrays for implot
    size_t n = m_displayBuf.size();
    std::vector<float> times(n), rawX(n), filtX(n), rawY(n), filtY(n);
    size_t idx = 0;
    for (const auto& pt : m_displayBuf) {
        times[idx] = static_cast<float>(pt.time);
        rawX[idx]  = pt.rawX;
        filtX[idx] = pt.filtX;
        rawY[idx]  = pt.rawY;
        filtY[idx] = pt.filtY;
        ++idx;
    }

    // Auto-scroll: keep last N seconds in view
    if (m_autoScroll && n > 0) {
        m_plotTimeMax = times[n - 1] + 0.1f;
        m_plotTimeMin = m_plotTimeMax - m_config->config().gui.plotHistoryS;
        if (m_plotTimeMin < 0.0f) m_plotTimeMin = 0.0f;
    }

    float plotHeight = (ImGui::GetContentRegionAvail().y - 30) * 0.48f;

    // ── X-Axis Plot ─────────────────────────────────────────────────
    ImPlot::SetNextAxesToFit();
    if (ImPlot::BeginPlot("##PlotX", ImVec2(-1, plotHeight),
            ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        ImPlot::SetupAxes("Time (s)", "dx (px)",
            ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoTickLabels,
            ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, m_plotTimeMin, m_plotTimeMax, m_autoScroll ? ImGuiCond_Always : ImGuiCond_Once);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.1f");

        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.35f, 0.35f, 0.6f), 1.0f);
        ImPlot::PlotLine("Raw X", times.data(), rawX.data(), static_cast<int>(n));

        ImPlot::SetNextLineStyle(ImVec4(0.25f, 0.85f, 1.0f, 1.0f), 1.5f);
        ImPlot::PlotLine("Filtered X", times.data(), filtX.data(), static_cast<int>(n));

        ImPlot::EndPlot();
    }

    // ── Y-Axis Plot ─────────────────────────────────────────────────
    ImPlot::SetNextAxesToFit();
    if (ImPlot::BeginPlot("##PlotY", ImVec2(-1, plotHeight),
            ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        ImPlot::SetupAxes("Time (s)", "dy (px)",
            ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoTickLabels,
            ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, m_plotTimeMin, m_plotTimeMax, m_autoScroll ? ImGuiCond_Always : ImGuiCond_Once);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.1f");

        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.35f, 0.35f, 0.6f), 1.0f);
        ImPlot::PlotLine("Raw Y", times.data(), rawY.data(), static_cast<int>(n));

        ImPlot::SetNextLineStyle(ImVec4(0.25f, 0.85f, 1.0f, 1.0f), 1.5f);
        ImPlot::PlotLine("Filtered Y", times.data(), filtY.data(), static_cast<int>(n));

        ImPlot::EndPlot();
    }

    // Controls
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_displayBuf.clear();
        m_ringBuffer.reset();
    }
    ImGui::SameLine();
    ImGui::Text("Points: %zu", n);
}

// ---------------------------------------------------------------------------
// renderStatsPanel — Right-side statistics
// ---------------------------------------------------------------------------
void GUIApp::renderStatsPanel() {
    auto& hm = m_input->hookManager();
    const auto& profile = m_profiles->active();

    ImGui::TextUnformatted("Session Statistics");
    ImGui::Separator();

    ImGui::Text("Total Events:  %llu", hm.totalEvents());
    ImGui::Text("Filtered:      %llu", hm.filteredEvents());
    ImGui::Text("Drop Rate:     %.1f%%",
        hm.totalEvents() > 0
            ? 100.0 * static_cast<double>(hm.filteredEvents()) / hm.totalEvents()
            : 0.0);

    ImGui::Spacing();
    ImGui::TextUnformatted("Performance");
    ImGui::Separator();

    ImGui::Text("Hook Latency:");
    ImGui::SameLine();
    widgets::LatencyDisplay(hm.avgLatencyUs());

    ImGui::Spacing();
    ImGui::TextUnformatted("Movement Analysis");
    ImGui::Separator();

    ImGui::Text("Avg Raw Speed:     %.1f px/s", profile.avgRawSpeed);
    ImGui::Text("Avg Filtered Speed: %.1f px/s", profile.avgFilteredSpeed);

    double speedRatio = profile.avgRawSpeed > 1.0
        ? profile.avgFilteredSpeed / profile.avgRawSpeed * 100.0 : 100.0;
    ImGui::Text("Smooth Ratio:      %.0f%%", speedRatio);

    ImGui::Text("Tremor Freq:       %.1f Hz", profile.tremorFreqHz);
    ImGui::Text("Overshoot Ratio:   %.1f%%", profile.overshootRatio * 100.0);

    ImGui::Spacing();
    ImGui::TextUnformatted("Session");
    ImGui::Separator();

    ImGui::Text("Duration:   %.0f s", m_profiles->sessionDurationS());
    ImGui::Text("Samples:    %zu", m_profiles->sampleCount());

    ImGui::Spacing();
    if (ImGui::Button("Recalculate Profile", ImVec2(-1, 0))) {
        m_profiles->recomputeParameters();
        pushProfileToPipeline();
    }

    // Plot history slider
    ImGui::Spacing();
    float hist = m_config->config().gui.plotHistoryS;
    if (ImGui::SliderFloat("History", &hist, 0.5f, 10.0f, "%.1f s")) {
        m_config->configMutable().gui.plotHistoryS = hist;
    }
}

// ============================================================================
// Profile Tab — Radar chart + editor
// ============================================================================
void GUIApp::showProfileTab() {
    auto& profile = m_profiles->active();

    // ── Refresh profile list if needed ──────────────────────────────
    if (m_profileListDirty) {
        m_profileList = m_profiles->listProfiles();
        m_profileListDirty = false;
        // Find current profile in list
        m_selectedProfileIdx = -1;
        for (size_t i = 0; i < m_profileList.size(); ++i) {
            if (m_profileList[i] == profile.name) {
                m_selectedProfileIdx = static_cast<int>(i);
                break;
            }
        }
    }

    // ── Lazy init editor from active profile ────────────────────────
    if (!m_profileDirty) {
        strncpy_s(m_profileNameBuf, profile.name.c_str(), sizeof(m_profileNameBuf) - 1);
        strncpy_s(m_profileGameBuf, profile.game.c_str(), sizeof(m_profileGameBuf) - 1);
        m_editTremorSev     = static_cast<float>(profile.tremorSeverity);
        m_editOvershoot     = static_cast<float>(profile.overshootTendency);
        m_editFatigue       = static_cast<float>(profile.fatigueRate);
        m_editReactionDelay = static_cast<float>(profile.reactionDelayMs);
        m_editTremorSevX    = static_cast<float>(profile.tremorSeverityX);
        m_editTremorSevY    = static_cast<float>(profile.tremorSeverityY);
        m_editDPI           = profile.preferredDPI;
        m_editSensitivity   = static_cast<float>(profile.preferredSensitivity);
        m_editPollingHz      = static_cast<int>(profile.preferredHz);
        m_profileDirty      = true;  // mark as synced
    }

    // ── Left column: Radar chart ────────────────────────────────────
    ImGui::BeginChild("ProfileLeft", ImVec2(280, 0), ImGuiChildFlags_Border);

    // Build radar chart values (normalized 0..1 from 0..10 scale)
    float radarValues[5];
    radarValues[0] = std::clamp(static_cast<float>(profile.tremorSeverity)    / 10.0f, 0.0f, 1.0f);
    radarValues[1] = std::clamp(static_cast<float>(profile.overshootTendency) / 10.0f, 0.0f, 1.0f);
    radarValues[2] = std::clamp(static_cast<float>(profile.fatigueRate)       / 10.0f, 0.0f, 1.0f);
    radarValues[3] = std::clamp(static_cast<float>(profile.reactionDelayMs)   / 200.0f, 0.0f, 1.0f);
    radarValues[4] = 1.0f - std::clamp(static_cast<float>(profile.tremorFreqHz) / 15.0f, 0.0f, 1.0f);

    const char* radarLabels[5] = { "Tremor", "Overshoot", "Fatigue", "Reaction", "Stability" };

    widgets::DrawRadarChart("Motor Profile", radarValues, radarLabels, 240.0f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Recommended Parameters");
    ImGui::Spacing();

    ImGui::Text("Beta:        %.2f", profile.recommendedBeta);
    ImGui::Text("Min Cutoff:  %.2f Hz", profile.recommendedMinCutoff);
    ImGui::Text("Speed Coeff: %.2f", profile.recommendedSpeedCoeff);

    ImGui::Spacing();
    if (ImGui::Button("Apply to Filter", ImVec2(-1, 0))) {
        pushProfileToPipeline();
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right column: Profile editor ────────────────────────────────
    ImGui::BeginChild("ProfileRight", ImVec2(0, 0), ImGuiChildFlags_Border);

    // Profile selector
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("Active Profile",
            m_selectedProfileIdx >= 0 ? m_profileList[m_selectedProfileIdx].c_str() : "None")) {
        for (int i = 0; i < static_cast<int>(m_profileList.size()); ++i) {
            bool isSelected = (i == m_selectedProfileIdx);
            if (ImGui::Selectable(m_profileList[i].c_str(), isSelected)) {
                m_selectedProfileIdx = i;
                m_profiles->loadProfile(m_profileList[i]);
                m_profileDirty = false;  // force re-sync from loaded profile
                m_profileListDirty = true;
                pushProfileToPipeline();
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        m_profileListDirty = true;
    }

    ImGui::Spacing();
    widgets::SectionHeader("Identity");

    ImGui::SetNextItemWidth(250);
    ImGui::InputText("Name", m_profileNameBuf, sizeof(m_profileNameBuf));
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Game", m_profileGameBuf, sizeof(m_profileGameBuf));

    widgets::SectionHeader("Motor Assessment (0 = none, 10 = severe)");

    ImGui::SetNextItemWidth(300);
    bool changed = false;
    changed |= ImGui::SliderFloat("Tremor Severity", &m_editTremorSev, 0.0f, 10.0f, "%.1f");
    widgets::HelpMarker("Overall hand tremor intensity. Higher = more smoothing needed.");

    ImGui::SetNextItemWidth(300);
    changed |= ImGui::SliderFloat("Tremor X (Horizontal)", &m_editTremorSevX, 0.0f, 10.0f, "%.1f");
    widgets::HelpMarker("Horizontal component of tremor. Often worse for wrist aimers.");

    ImGui::SetNextItemWidth(300);
    changed |= ImGui::SliderFloat("Tremor Y (Vertical)", &m_editTremorSevY, 0.0f, 10.0f, "%.1f");
    widgets::HelpMarker("Vertical component. Typically less severe than horizontal.");

    ImGui::SetNextItemWidth(300);
    changed |= ImGui::SliderFloat("Overshoot Tendency", &m_editOvershoot, 0.0f, 10.0f, "%.1f");
    widgets::HelpMarker("Tendency to flick past the target. Higher = more speed adaptation needed.");

    ImGui::SetNextItemWidth(300);
    changed |= ImGui::SliderFloat("Fatigue Rate", &m_editFatigue, 0.0f, 10.0f, "%.1f");
    widgets::HelpMarker("How fast aim accuracy degrades over a gaming session.");

    ImGui::SetNextItemWidth(300);
    changed |= ImGui::SliderFloat("Reaction Delay", &m_editReactionDelay, 0.0f, 200.0f, "%.0f ms");
    widgets::HelpMarker("Additional delay beyond baseline (~200ms). Used for predictive lookahead.");

    widgets::SectionHeader("Mouse Settings");

    ImGui::SetNextItemWidth(150);
    changed |= ImGui::SliderInt("DPI", &m_editDPI, 200, 3200);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    changed |= ImGui::SliderFloat("Sensitivity", &m_editSensitivity, 0.1f, 10.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    changed |= ImGui::SliderInt("Polling Hz", &m_editPollingHz, 125, 8000);

    if (changed) {
        m_profileDirty = false;  // Will re-sync if "Save" is clicked
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Action buttons ──────────────────────────────────────────────
    if (ImGui::Button("Save Profile", ImVec2(110, 0))) {
        // Sync editor → active profile
        auto& p = m_profiles->active();
        p.name              = m_profileNameBuf;
        p.game              = m_profileGameBuf;
        p.tremorSeverity    = m_editTremorSev;
        p.tremorSeverityX   = m_editTremorSevX;
        p.tremorSeverityY   = m_editTremorSevY;
        p.overshootTendency = m_editOvershoot;
        p.fatigueRate       = m_editFatigue;
        p.reactionDelayMs   = m_editReactionDelay;
        p.preferredDPI      = m_editDPI;
        p.preferredSensitivity = m_editSensitivity;
        p.preferredHz       = static_cast<double>(m_editPollingHz);

        m_profiles->saveProfile();
        m_profiles->recomputeParameters();
        pushProfileToPipeline();
        m_profileListDirty = true;
        m_profileDirty     = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(80, 0))) {
        m_profiles->loadProfile(m_profileNameBuf);
        m_profileDirty = false;
        pushProfileToPipeline();
    }

    ImGui::SameLine();
    if (ImGui::Button("New Blank", ImVec2(100, 0))) {
        auto blank = m_profiles->createBlank("new_user", "unknown");
        m_profiles->active() = blank;
        m_profileDirty = false;
        m_profileListDirty = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(70, 0))) {
        if (m_selectedProfileIdx >= 0 &&
            static_cast<size_t>(m_selectedProfileIdx) < m_profileList.size()) {
            m_profiles->deleteProfile(m_profileList[m_selectedProfileIdx]);
            m_profileListDirty = true;
            m_profileDirty = false;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Calibration section ─────────────────────────────────────────
    ImGui::TextUnformatted("Motor Calibration");

    if (!m_calibration) {
        // Lazy-create the wizard
        m_calibration = new CalibrationWizard();
    }

    if (m_calibration->isRunning()) {
        // Show progress and cancel button
        float prog = m_calibration->progress() * 100.0f;
        ImGui::Text("Phase: %s (%.0f%%)", m_calibration->phaseName(), prog);
        ImGui::ProgressBar(m_calibration->progress(), ImVec2(-1, 0));

        if (ImGui::Button("Cancel Calibration", ImVec2(160, 0))) {
            m_calibration->stop();
        }

        // Render the calibration canvas
        ImGui::Spacing();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize(
            ImGui::GetContentRegionAvail().x,
            ImGui::GetContentRegionAvail().y - 40);
        if (canvasSize.y < 200) canvasSize.y = 200;

        ImGui::InvisibleButton("CalCanvas", canvasSize);
        m_calibration->render(canvasPos, canvasSize);

        // Handle completion
        if (m_calibration->isComplete()) {
            if (ImGui::Button("Apply to Profile", ImVec2(180, 0))) {
                m_calibration->applyToProfile(m_profiles->active());
                m_profiles->recomputeParameters();
                pushProfileToPipeline();
                m_profileDirty = false;
                m_calibration->stop();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(100, 0))) {
                m_calibration->stop();
            }
        }
    } else {
        ImGui::TextWrapped(
            "Run a guided calibration to assess your tremor severity, "
            "overshoot tendency, fatigue rate, and reaction time. "
            "Results auto-tune the filter parameters for your needs."
        );
        ImGui::Spacing();
        if (ImGui::Button("Start Calibration", ImVec2(180, 30))) {
            m_calibration->start();
        }
    }

    ImGui::EndChild();
}

// ============================================================================
// Settings Tab — Filter parameters + advanced
// ============================================================================
void GUIApp::showSettingsTab() {
    ImGui::BeginChild("SettingsPanel", ImVec2(0, 0), ImGuiChildFlags_Border);

    // ── Filter type selector ─────────────────────────────────────────
    ImGui::SetNextItemWidth(200);
    const char* filterTypes[] = { "OneEuro", "EMA", "Kalman", "None (Passthrough)" };
    if (ImGui::Combo("Filter Type", &m_editFilterType, filterTypes, 4)) {
        // Immediate apply
        m_input->setFilterType(static_cast<FilterType>(m_editFilterType));
        m_config->configMutable().filter.type =
            m_editFilterType == 0 ? "one_euro" :
            m_editFilterType == 1 ? "ema" :
            m_editFilterType == 2 ? "kalman" : "none";
    }

    ImGui::Spacing();

    // ── Per-filter parameters ────────────────────────────────────────
    switch (m_editFilterType) {
    case 0: { // OneEuro
        widgets::SectionHeader("OneEuro Filter Parameters");

        bool oeChanged = false;
        ImGui::SetNextItemWidth(350);
        oeChanged |= widgets::SliderWithHelp("Min Cutoff (Hz)", &m_editMinCutoff,
            0.01f, 10.0f,
            "Minimum cutoff frequency. Lower values = heavier smoothing.\n"
            "Typical: 1.0-2.0 Hz for moderate tremor, 0.3-0.8 Hz for severe.");

        ImGui::SetNextItemWidth(350);
        oeChanged |= widgets::SliderWithHelp("Beta", &m_editBeta,
            0.0f, 200.0f,
            "Speed coefficient. Controls how much the cutoff rises during fast movement.\n"
            "0 = no speed adaptation (constant smoothing).\n"
            "High values (50-200) = minimal lag during flicks, but less smoothing at speed.\n"
            "Typical: 5-50 for tremor, 0-5 for overshoot compensation.");

        ImGui::SetNextItemWidth(350);
        oeChanged |= widgets::SliderWithHelp("Derivative Cutoff (Hz)", &m_editDCutoff,
            0.1f, 5.0f,
            "Cutoff for the speed (derivative) low-pass filter.\n"
            "Lower = smoother speed estimate = less jittery adaptation.\n"
            "1.0 Hz is standard.");

        ImGui::SetNextItemWidth(350);
        oeChanged |= widgets::SliderWithHelp("Speed Coefficient", &m_editSpeedCoeff,
            0.1f, 5.0f,
            "Multiplier on speed before computing adaptive cutoff.\n"
            ">1.0 amplifies the speed effect (more responsive flicks).\n"
            "<1.0 dampens it (more consistent smoothing across speeds).");

        if (oeChanged) {
            m_input->hookManager().oneEuro().setParams(
                m_editMinCutoff, m_editBeta, m_editDCutoff, m_editSpeedCoeff);
            m_config->setOneEuroParams(m_editMinCutoff, m_editBeta,
                                       m_editDCutoff, m_editSpeedCoeff);
        }
        break;
    }
    case 1: { // EMA
        widgets::SectionHeader("EMA Filter Parameters");

        ImGui::SetNextItemWidth(350);
        if (widgets::SliderWithHelp("Alpha", &m_editEMAlpha, 0.01f, 0.99f,
            "Smoothing factor. Higher = less smoothing, more responsive.\n"
            "0.5 = equal weight to new and old values.\n"
            "0.1 = heavy smoothing. 0.9 = very light smoothing.",
            "%.3f")) {
            m_input->hookManager().ema().setAlpha(m_editEMAlpha);
            m_config->setEMAParams(m_editEMAlpha);
        }
        break;
    }
    case 2: { // Kalman
        widgets::SectionHeader("Kalman Filter Parameters");

        bool kChanged = false;
        ImGui::SetNextItemWidth(350);
        kChanged |= widgets::SliderWithHelp("Process Noise (Q)", &m_editKQ,
            0.0001f, 10.0f,
            "How much the true mouse position changes per step.\n"
            "Higher = filter trusts new measurements more = less lag.\n"
            "Lower = filter trusts its model more = smoother output.",
            "%.4f");

        ImGui::SetNextItemWidth(350);
        kChanged |= widgets::SliderWithHelp("Measurement Noise (R)", &m_editKR,
            0.0001f, 10.0f,
            "How noisy the raw measurements are.\n"
            "Higher = filter smooths more aggressively.\n"
            "The ratio Q/R determines steady-state behavior.",
            "%.4f");

        if (kChanged) {
            m_input->hookManager().kalman().setParams(m_editKQ, m_editKR);
            m_config->setKalmanParams(m_editKQ, m_editKR);
        }
        break;
    }
    default: {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1.0f),
            "Passthrough mode — no filtering applied.");
        break;
    }
    }

    ImGui::Spacing();
    widgets::SectionHeader("Advanced");

    bool advChanged = false;
    ImGui::SetNextItemWidth(350);
    advChanged |= ImGui::SliderInt("Deadzone (px)", &m_editDeadzone, 0, 50);
    widgets::HelpMarker("Minimum movement per axis before filtering engages.\n"
                        "Suppresses micro-tremor that doesn't move the cursor.\n"
                        "Set to 0 for no deadzone.");

    ImGui::SetNextItemWidth(350);
    advChanged |= ImGui::SliderInt("Max Delta (px)", &m_editMaxDelta, 10, 500);
    widgets::HelpMarker("Maximum allowed mouse delta per event.\n"
                        "Prevents coordinate jumps from multi-monitor transitions.");

    if (advChanged) {
        m_input->hookManager().setDeadzone(m_editDeadzone);
        m_input->hookManager().setMaxDelta(m_editMaxDelta);
        m_config->configMutable().advanced.deadzonePx = m_editDeadzone;
        m_config->configMutable().advanced.maxDeltaPx = m_editMaxDelta;
    }

    ImGui::Spacing();
    widgets::SectionHeader("Hotkeys");

    ImGui::SetNextItemWidth(200);
    ImGui::InputInt("Primary Hotkey (VK code)", &m_editHotkey);
    widgets::HelpMarker("Virtual key code. 0x90 = NumLock, 0x77 = F8.");

    ImGui::SetNextItemWidth(200);
    ImGui::InputInt("Alternate Hotkey (VK code)", &m_editHotkeyAlt);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Save Config to Disk", ImVec2(180, 0))) {
        m_config->configMutable().global.hotkey    = m_editHotkey;
        m_config->configMutable().global.hotkeyAlt = m_editHotkeyAlt;
        m_config->save();
        LOG_INFO("Config saved from GUI");
    }

    ImGui::SameLine();
    if (ImGui::Button("Reload from Disk", ImVec2(160, 0))) {
        m_config->load();
        pullSettingsFromConfig();
        pushSettingsToPipeline();
        LOG_INFO("Config reloaded from disk");
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults", ImVec2(160, 0))) {
        m_config->generateDefault();
        pullSettingsFromConfig();
        pushSettingsToPipeline();
        LOG_INFO("Config reset to defaults");
    }

    ImGui::EndChild();
}

// ============================================================================
// Games Tab — Raw Input injection for modern FPS titles
// ============================================================================
void GUIApp::showGamesTab() {
    ImGui::BeginChild("GamesPanel", ImVec2(0, 0), ImGuiChildFlags_Border);

    // ── Anti-cheat warning ────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
    ImGui::TextWrapped(
        "WARNING: DLL injection + API hooking is detectable by anti-cheat systems. "
        "Use only in single-player, community servers, or games that explicitly "
        "allow input modification for accessibility purposes."
    );
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();

    if (!m_injector) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "GameInjector not initialized. Restart AetherAim as Administrator.");
        ImGui::EndChild();
        return;
    }

    // ── Scan interval timer ──────────────────────────────────────────
    m_gameScanTimer += ImGui::GetIO().DeltaTime;
    static std::vector<GameStatus> cachedStatus;

    if (m_gameScanTimer >= GAME_SCAN_INTERVAL || cachedStatus.empty()) {
        cachedStatus = m_injector->scanGames();
        m_gameScanTimer = 0.0f;
    }

    // ── Shared memory status ──────────────────────────────────────────
    bool sharedMemOk = m_injector->hasSharedMemory();
    ImGui::TextColored(sharedMemOk ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                   : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
        sharedMemOk ? "Shared Memory: ACTIVE" : "Shared Memory: NOT CREATED");

    ImGui::SameLine();
    if (ImGui::Button("Create Shared Memory")) {
        m_injector->createSharedMemory();
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh Scan")) {
        cachedStatus = m_injector->scanGames();
        m_gameScanTimer = 0.0f;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Detected Games:");
    ImGui::Spacing();

    if (cachedStatus.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "No supported games detected. Launch a game and click Refresh Scan.");
    }

    // ── Game list ─────────────────────────────────────────────────────
    for (const auto& status : cachedStatus) {
        ImGui::PushID(status.info.processName.c_str());

        // Convert wide names to UTF-8 for display
        char nameBuf[128] = {};
        char procBuf[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, status.info.name.c_str(), -1,
                            nameBuf, sizeof(nameBuf), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, status.info.processName.c_str(), -1,
                            procBuf, sizeof(procBuf), nullptr, nullptr);

        // Status color
        ImVec4 statusColor;
        const char* statusText;
        switch (status.state) {
        case InjectionState::Injected:
            statusColor = ImVec4(0.3f, 0.95f, 0.3f, 1.0f);
            statusText  = "INJECTED";
            break;
        case InjectionState::Running:
            statusColor = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
            statusText  = "RUNNING";
            break;
        case InjectionState::Blocked:
            statusColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            statusText  = "BLOCKED";
            break;
        case InjectionState::Error:
            statusColor = ImVec4(1.0f, 0.5f, 0.2f, 1.0f);
            statusText  = "ERROR";
            break;
        default:
            statusColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            statusText  = "NOT RUNNING";
            break;
        }

        ImGui::Text("%s  [%s]", nameBuf, procBuf);
        ImGui::SameLine(300);
        ImGui::TextColored(statusColor, "%s", statusText);

        // Action button
        ImGui::SameLine(420);
        if (status.state == InjectionState::Running) {
            if (ImGui::Button("Inject")) {
                if (m_injector->inject(status.pid)) {
                    cachedStatus = m_injector->scanGames();
                }
            }
        } else if (status.state == InjectionState::Injected) {
            if (ImGui::Button("Eject")) {
                if (m_injector->eject(status.pid)) {
                    cachedStatus = m_injector->scanGames();
                }
            }

            // Show stats
            ImGui::SameLine();
            ImGui::Text("  Events: %llu | Filtered: %llu | Latency: %.0f ns",
                        status.totalEvents, status.filteredEvents,
                        status.avgLatencyNs);
        } else if (status.state == InjectionState::Blocked) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.3f, 1.0f),
                "(Anti-cheat blocking access)");
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped(
        "How it works: AetherAimHook.dll is injected into the game process. "
        "It hooks GetRawInputData() — the Windows API games use to read mouse "
        "input directly from the hardware. Before the game sees the mouse data, "
        "the OneEuro filter smooths the raw delta values. Filter parameters are "
        "synced in real-time via shared memory."
    );

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
        "DLL path: %s",
        [&]() -> std::string {
            auto& path = m_injector->dllPath();
            return std::string(path.begin(), path.end());
        }().c_str());

    ImGui::EndChild();
}

// ============================================================================
// About Tab
// ============================================================================
void GUIApp::showAboutTab() {
    ImGui::BeginChild("AboutPanel", ImVec2(0, 0), ImGuiChildFlags_Border);

    ImGui::TextUnformatted("AetherAim v1.0.0");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped(
        "AetherAim is an open-source FPS accessibility assistant designed "
        "for gamers with motor disabilities including tremor, Parkinson's, "
        "muscle fatigue, and overshoot tendencies.\n\n"

        "It applies real-time adaptive filtering to mouse input, smoothing "
        "unintentional hand movements while preserving intentional aim.\n\n"

        "Supported Games: CS2, Valorant, Overwatch 2, Apex Legends, and more.\n\n"

        "Technologies: C++20, DirectX 11, Dear ImGui, OneEuro Filter\n"
        "Build: "
        #ifdef _DEBUG
        "Debug"
        #else
        "Release"
        #endif
        " | Compiler: MSVC | Platform: Windows 10/11"
    );

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Keyboard Shortcuts:");
    ImGui::BulletText("NumLock / F8 — Toggle filter on/off");
    ImGui::BulletText("Alt+F4 — Exit application");

    ImGui::Spacing();
    if (ImGui::Button("ImGui Demo", ImVec2(130, 0))) m_showImGuiDemo = !m_showImGuiDemo;
    ImGui::SameLine();
    if (ImGui::Button("ImPlot Demo", ImVec2(130, 0))) m_showImPlotDemo = !m_showImPlotDemo;

    ImGui::EndChild();
}

// ============================================================================
// Helpers: Config ↔ Pipeline synchronization
// ============================================================================

void GUIApp::pullSettingsFromConfig() {
    const auto& cfg = m_config->config();

    // Filter type
    m_editFilterType =
        cfg.filter.type == "ema"    ? 1 :
        cfg.filter.type == "kalman" ? 2 :
        cfg.filter.type == "none"   ? 3 : 0;

    // OneEuro
    m_editMinCutoff  = static_cast<float>(cfg.filter.oneEuro.minCutoff);
    m_editBeta       = static_cast<float>(cfg.filter.oneEuro.beta);
    m_editDCutoff    = static_cast<float>(cfg.filter.oneEuro.dCutoff);
    m_editSpeedCoeff = static_cast<float>(cfg.filter.oneEuro.speedCoeff);

    // EMA
    m_editEMAlpha = static_cast<float>(cfg.filter.ema.alpha);

    // Kalman
    m_editKQ = static_cast<float>(cfg.filter.kalman.processNoise);
    m_editKR = static_cast<float>(cfg.filter.kalman.measurementNoise);

    // Advanced
    m_editDeadzone = cfg.advanced.deadzonePx;
    m_editMaxDelta = cfg.advanced.maxDeltaPx;

    // Hotkeys
    m_editHotkey    = cfg.global.hotkey;
    m_editHotkeyAlt = cfg.global.hotkeyAlt;
}

void GUIApp::pushSettingsToPipeline() {
    InputProcessor::FilterProfile fp;
    fp.type = static_cast<FilterType>(m_editFilterType);
    fp.minCutoff  = m_editMinCutoff;
    fp.beta       = m_editBeta;
    fp.dCutoff    = m_editDCutoff;
    fp.speedCoeff = m_editSpeedCoeff;
    fp.emaAlpha   = m_editEMAlpha;
    fp.kProcessNoise     = m_editKQ;
    fp.kMeasurementNoise = m_editKR;
    fp.deadzone   = m_editDeadzone;
    fp.maxDelta   = m_editMaxDelta;

    m_input->applyProfile(fp);
}

void GUIApp::pushProfileToPipeline() {
    m_profiles->applyToProcessor(*m_input);
    // Also sync the GUI editor state
    m_profileDirty = false;
}

// ============================================================================
// Window visibility helpers
// ============================================================================
void GUIApp::hideWindow() {
    if (m_hWnd) ShowWindow(m_hWnd, SW_HIDE);
}

void GUIApp::showWindow() {
    if (m_hWnd) {
        ShowWindow(m_hWnd, SW_SHOW);
        SetForegroundWindow(m_hWnd);
    }
    m_minimizedToTray = false;
}

void GUIApp::toggleVisibility() {
    if (isVisible()) {
        hideWindow();
        m_minimizedToTray = true;
    } else {
        showWindow();
    }
}

bool GUIApp::isVisible() const {
    return m_hWnd && IsWindowVisible(m_hWnd);
}

void GUIApp::minimizeToTray() {
    hideWindow();
    m_minimizedToTray = true;

    // Show first-time balloon notification
    if (m_tray) {
        m_tray->showBalloon(
            L"AetherAim",
            L"AetherAim is still running in the system tray.\n"
            L"Right-click the icon to show the window or exit.",
            NIIF_INFO
        );
    }
}

// ---------------------------------------------------------------------------
// handleTrayMessage — process tray icon context menu clicks
// Returns true if the message was handled.
// ---------------------------------------------------------------------------
bool GUIApp::handleTrayMessage(WPARAM wParam, LPARAM lParam) {
    switch (LOWORD(lParam)) {
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        // Show context menu
        if (m_tray) {
            m_tray->showContextMenu(
                m_input ? m_input->isEnabled() : false,
                isVisible()
            );
        }
        return true;

    case WM_LBUTTONDBLCLK:
        // Double-click: toggle window visibility
        toggleVisibility();
        return true;

    default:
        break;
    }

    // Handle context menu commands (wParam is the menu item ID)
    switch (LOWORD(wParam)) {
    case SystemTray::MENU_TOGGLE_FILTER:
        if (m_input) {
            m_input->toggle();
            LOG_INFO("Filter toggled via tray menu: {}",
                     m_input->isEnabled() ? "ON" : "OFF");
        }
        return true;

    case SystemTray::MENU_SHOW_HIDE:
        toggleVisibility();
        return true;

    case SystemTray::MENU_EXIT:
        requestShutdown();
        // Post WM_QUIT to break the message loop
        if (m_hWnd) PostMessageW(m_hWnd, WM_QUIT, 0, 0);
        return true;

    default:
        break;
    }

    return false;
}

// ---------------------------------------------------------------------------
// checkAutoSave — periodic profile auto-save
// ---------------------------------------------------------------------------
void GUIApp::checkAutoSave() {
    if (!m_profiles || !m_config) return;

    int intervalS = m_config->config().profiles.autoSaveIntervalS;
    if (intervalS <= 0) return;

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_lastAutoSave).count();

    if (elapsed >= static_cast<double>(intervalS)) {
        m_profiles->recomputeParameters();
        m_profiles->saveProfile();
        m_lastAutoSave = now;
        LOG_DEBUG("Auto-saved profile '{}'", m_profiles->active().name);
    }
}

// ---------------------------------------------------------------------------
// checkConfigReload — hot reload config from disk
// ---------------------------------------------------------------------------
void GUIApp::checkConfigReload() {
    if (!m_config) return;

    if (m_config->reloadIfChanged()) {
        // Config changed on disk — resync GUI editor state
        pullSettingsFromConfig();
        pushSettingsToPipeline();
        LOG_INFO("Config hot-reloaded from disk");
    }
}

// ---------------------------------------------------------------------------
// checkHookHealth — detect and recover from hook loss
//
// If another application steals the low-level mouse hook (e.g. other
// accessibility tools, macro software, some games), our hook may be
// silently detached without notification. The heartbeat counter detects
// this: if it stops incrementing for 3+ seconds while filtering is
// enabled, the hook has likely been lost.
// ---------------------------------------------------------------------------
void GUIApp::checkHookHealth() {
    if (!m_input || !m_input->isEnabled()) return;

    auto& hm = m_input->hookManager();
    static uint64_t lastHeartbeat = 0;
    static auto lastCheck = std::chrono::steady_clock::now();

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - lastCheck).count();
    if (elapsed < 3.0) return;  // Check every 3 seconds

    uint64_t current = hm.heartbeat();
    if (current == lastHeartbeat && lastHeartbeat > 0) {
        // No mouse events received for 3+ seconds while filter is enabled
        // → hook is likely detached
        LOG_WARN("Hook heartbeat stalled (last={}) — attempting reinstall",
                 lastHeartbeat);

        m_input->stop();
        Sleep(100);  // Let the system clean up
        if (m_input->start()) {
            LOG_INFO("Hook reinstalled successfully");
        } else {
            LOG_ERROR("Hook reinstall failed — may need app restart");
        }
    }
    lastHeartbeat = current;
    lastCheck = now;
}

// ============================================================================
// Win32 Window Procedure
// ============================================================================
LRESULT CALLBACK GUIApp::wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Let ImGui handle input first
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    auto* self = reinterpret_cast<GUIApp*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(pCreate->lpCreateParams));
        return 0;
    }

    case WM_SIZE: {
        if (self && self->m_swapChain && self->m_d3dDevice && wParam != SIZE_MINIMIZED) {
            if (self->m_renderTarget) { self->m_renderTarget->Release(); self->m_renderTarget = nullptr; }
            self->m_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                              DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* backBuffer = nullptr;
            self->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         reinterpret_cast<void**>(&backBuffer));
            if (backBuffer) {
                self->m_d3dDevice->CreateRenderTargetView(backBuffer, nullptr,
                                                          &self->m_renderTarget);
                backBuffer->Release();
            }
        }
        return 0;
    }

    case WM_CLOSE:
        // Minimize to tray instead of closing
        if (self) {
            self->minimizeToTray();
        }
        return 0;  // Swallow WM_CLOSE — don't pass to DefWindowProc

    case WM_TRAY_CALLBACK:
        // Delegate to GUIApp handler
        if (self && self->handleTrayMessage(wParam, lParam)) {
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace aether
