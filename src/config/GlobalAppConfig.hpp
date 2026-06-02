#pragma once
// ============================================================================
// GlobalAppConfig.hpp — Central Configuration Data Structures
//
// Pure data types with nlohmann::json serialization. No I/O, no logic.
// Consumed by: ConfigManager (persistence), GUI (display/edit),
//              InputProcessor (bulk parameter push).
//
// All numeric ranges documented inline — GUI sliders clamp to these.
// ============================================================================

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace aether {

// ============================================================================
// Per-filter parameter groups
// ============================================================================

struct FilterParams_OneEuro {
    double minCutoff  = 1.2;   // [0.01 .. 10.0]  Hz  — lower = smoother
    double beta       = 0.0;   // [0.0  .. 200.0]     — 0 = no speed adaptation
    double dCutoff    = 1.0;   // [0.1  .. 5.0]   Hz  — derivative LP cutoff
    double speedCoeff = 1.0;   // [0.1  .. 5.0]       — speed sensitivity multiplier
    double minJitter  = 0.001; // [1e-6 .. 0.1]   s   — minimum dt clamp

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FilterParams_OneEuro,
        minCutoff, beta, dCutoff, speedCoeff, minJitter)
};

struct FilterParams_EMA {
    double alpha = 0.6;  // [0.01 .. 0.99]  — higher = less smoothing, more responsive

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FilterParams_EMA, alpha)
};

struct FilterParams_Kalman {
    double processNoise     = 0.01;  // [1e-6 .. 10.0]  — Q, higher = more responsive
    double measurementNoise = 0.1;   // [1e-6 .. 10.0]  — R, higher = more smoothing
    double initialError     = 1.0;   // [0.01 .. 100.0]  — P0, higher = faster convergence

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FilterParams_Kalman,
        processNoise, measurementNoise, initialError)
};

// ============================================================================
// Global settings
// ============================================================================

struct GlobalSettings {
    bool    enabled     = false;      // Filter on at startup?
    int32_t hotkey      = 0x90;       // VK_NUMLOCK (144)
    int32_t hotkeyAlt   = 0x77;       // VK_F8 (119)
    bool    startMinimized = false;   // Start GUI minimized to tray?
    bool    runOnStartup = false;     // Auto-start with Windows?

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(GlobalSettings,
        enabled, hotkey, hotkeyAlt, startMinimized, runOnStartup)
};

// ============================================================================
// Filter selection + advanced tuning
// ============================================================================

struct FilterSettings {
    std::string type = "one_euro";    // "one_euro" | "ema" | "kalman" | "none"
    FilterParams_OneEuro oneEuro;
    FilterParams_EMA      ema;
    FilterParams_Kalman   kalman;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FilterSettings, type, oneEuro, ema, kalman)
};

struct AdvancedSettings {
    int32_t deadzonePx    = 0;      // [0 .. 50]  px — deadzone radius per axis
    int32_t maxDeltaPx    = 100;    // [10 .. 500] px — clamp to prevent coordinate-jump glitches
    int32_t pollingRateHz = 1000;   // [125 .. 8000] Hz — informational, for dt estimation
    bool    subPixelAccum = true;   // Enable fractional pixel accumulation

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AdvancedSettings,
        deadzonePx, maxDeltaPx, pollingRateHz, subPixelAccum)
};

// ============================================================================
// Profile discovery
// ============================================================================

struct ProfileSettings {
    std::string activeProfile = "default";
    std::string profileDir    = "./profiles/";
    bool        autoSave      = true;    // Auto-save profile on exit?
    int32_t     autoSaveIntervalS = 60;  // Periodic auto-save (seconds)

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProfileSettings,
        activeProfile, profileDir, autoSave, autoSaveIntervalS)
};

// ============================================================================
// GUI state (persisted across sessions)
// ============================================================================

struct GUISettings {
    int32_t windowX  = 100;
    int32_t windowY  = 100;
    int32_t windowW  = 900;
    int32_t windowH  = 700;
    bool    showRealtimePlot = true;
    bool    showRadarChart   = true;
    float   plotHistoryS     = 3.0f;   // Seconds of history to display

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(GUISettings,
        windowX, windowY, windowW, windowH,
        showRealtimePlot, showRadarChart, plotHistoryS)
};

// ============================================================================
// GlobalAppConfig — root aggregate
// ============================================================================

struct GlobalAppConfig {
    std::string       schemaVersion = "1.0";  // For future migration
    GlobalSettings    global;
    FilterSettings    filter;
    AdvancedSettings  advanced;
    ProfileSettings   profiles;
    GUISettings       gui;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(GlobalAppConfig,
        schemaVersion, global, filter, advanced, profiles, gui)
};

// ============================================================================
// Helper: convert FilterSettings.type string → FilterType enum
// ============================================================================
inline std::string filterTypeToString(int /*FilterType*/ type) {
    // Will be used with the actual FilterType enum from core/
    return "one_euro";
}

} // namespace aether
