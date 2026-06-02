// ============================================================================
// InputProcessor.cpp — Filter Strategy Orchestrator Implementation
// ============================================================================

#include "InputProcessor.hpp"
#include "utils/Logger.hpp"

namespace aether {

InputProcessor::InputProcessor() {
    LOG_DEBUG("InputProcessor created");
}

InputProcessor::~InputProcessor() {
    stop();
}

// ---------------------------------------------------------------------------
// Pipeline lifecycle
// ---------------------------------------------------------------------------
bool InputProcessor::start() {
    if (m_hookManager.isInstalled()) {
        LOG_WARN("Hook already running");
        return true;
    }
    bool ok = m_hookManager.install();
    if (ok) {
        LOG_INFO("Input pipeline started");
    }
    return ok;
}

void InputProcessor::stop() {
    if (m_hookManager.isInstalled()) {
        m_hookManager.uninstall();
        LOG_INFO("Input pipeline stopped");
    }
}

// ---------------------------------------------------------------------------
// Toggle
// ---------------------------------------------------------------------------
void InputProcessor::toggle() {
    setEnabled(!m_hookManager.isEnabled());
}

void InputProcessor::setEnabled(bool on) {
    m_hookManager.setEnabled(on);
    LOG_INFO("Filter {}", on ? "ENABLED" : "DISABLED");
}

bool InputProcessor::isEnabled() const {
    return m_hookManager.isEnabled();
}

// ---------------------------------------------------------------------------
// Filter switching
// ---------------------------------------------------------------------------
void InputProcessor::setFilterType(FilterType type) {
    m_hookManager.setFilterType(type);
}

// ---------------------------------------------------------------------------
// applyProfile — bulk parameter push from GUI / config
//
// This is the hot path for real-time parameter adjustment. All setters
// are lock-free (atomics + direct member writes from hook thread).
// The filter reset happens inside setFilterType if the type changes.
// ---------------------------------------------------------------------------
void InputProcessor::applyProfile(const FilterProfile& p) {
    // Switch filter type if needed (resets filter state internally)
    if (m_hookManager.getFilterType() != p.type) {
        m_hookManager.setFilterType(p.type);
    }

    // OneEuro parameters (safe: only read on hook thread)
    m_hookManager.oneEuro().setMinCutoff(p.minCutoff);
    m_hookManager.oneEuro().setBeta(p.beta);
    m_hookManager.oneEuro().setDCutoff(p.dCutoff);
    m_hookManager.oneEuro().setSpeedCoeff(p.speedCoeff);

    // EMA parameters
    m_hookManager.ema().setAlpha(p.emaAlpha);

    // Kalman parameters
    m_hookManager.kalman().setParams(p.kProcessNoise, p.kMeasurementNoise);

    // General settings (atomic — safe from any thread)
    m_hookManager.setDeadzone(p.deadzone);
    m_hookManager.setMaxDelta(p.maxDelta);

    LOG_DEBUG("Filter profile applied: type={}, minCutoff={:.2f}, beta={:.2f}, deadzone={}",
              static_cast<int>(p.type), p.minCutoff, p.beta, p.deadzone);
}

} // namespace aether
