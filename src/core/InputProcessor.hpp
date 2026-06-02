#pragma once
// ============================================================================
// InputProcessor.hpp — Filter Strategy Orchestrator
//
// Owns the MouseHookManager and provides a high-level API for:
//   - Starting / stopping the input pipeline
//   - Toggling filtering on/off (hotkey integration)
//   - Switching between filter backends at runtime
//   - Bulk-applying filter parameter profiles
//
// This is the primary interface consumed by main.cpp and GUIApp.
// ============================================================================

#include "MouseHookManager.hpp"
#include <memory>

namespace aether {

class InputProcessor {
public:
    InputProcessor();
    ~InputProcessor();

    // Non-copyable (owns OS hook)
    InputProcessor(const InputProcessor&)            = delete;
    InputProcessor& operator=(const InputProcessor&) = delete;

    // ── Pipeline lifecycle ────────────────────────────────────────────
    bool start();            // Install WH_MOUSE_LL hook
    void stop();             // Uninstall hook
    bool isRunning() const { return m_hookManager.isInstalled(); }

    // ── Filter toggle (hotkey) ────────────────────────────────────────
    void toggle();
    void setEnabled(bool on);
    bool isEnabled() const;

    // ── Filter strategy ───────────────────────────────────────────────
    void       setFilterType(FilterType type);
    FilterType getFilterType() const { return m_hookManager.getFilterType(); }

    // ── Direct access to hook manager (for GUI parameter binding) ─────
    MouseHookManager& hookManager() { return m_hookManager; }
    const MouseHookManager& hookManager() const { return m_hookManager; }

    // ── Bulk parameter profile application ────────────────────────────
    // This is the primary way the GUI pushes parameter changes.
    struct FilterProfile {
        FilterType type = FilterType::OneEuro;

        // OneEuro
        double minCutoff  = 1.2;
        double beta       = 0.0;
        double dCutoff    = 1.0;
        double speedCoeff = 1.0;

        // EMA
        double emaAlpha = 0.6;

        // Kalman
        double kProcessNoise     = 0.01;
        double kMeasurementNoise = 0.1;
        double kInitialError     = 1.0;

        // General
        int deadzone   = 0;
        int maxDelta   = 100;
    };

    void applyProfile(const FilterProfile& profile);

private:
    MouseHookManager m_hookManager;
};

} // namespace aether
