// ============================================================================
// OneEuroFilter.cpp — Implementation
// ============================================================================
#include "OneEuroFilter.hpp"

namespace aether {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
OneEuroFilter::OneEuroFilter(double minCutoff, double beta,
                               double dCutoff, double minJitter,
                               double speedCoeff) noexcept
    : m_minCutoff(std::max(minCutoff, 0.001))
    , m_beta(std::max(beta, 0.0))
    , m_dCutoff(std::max(dCutoff, 0.001))
    , m_minJitter(std::max(minJitter, 1e-6))
    , m_speedCoeff(std::max(speedCoeff, 0.01))
{}

// ---------------------------------------------------------------------------
// Reset — call on profile switch or enable/disable toggle
// ---------------------------------------------------------------------------
void OneEuroFilter::reset() noexcept {
    m_initialized  = false;
    m_prevRaw      = 0.0;
    m_prevFiltered = 0.0;
    m_prevDx       = 0.0;
}

// ---------------------------------------------------------------------------
// alpha — compute single-pole low-pass smoothing factor
//
// α = 1 / (1 + τ/dt)
// where τ = 1 / (2π · fc) is the time constant of the RC filter
//
// Clamped to (0, 1]: α→0 = infinite smoothing (frozen), α=1 = no smoothing
// Perf: 1 divide + 3 multiplies, ~2 CPU cycles on modern OoO cores
// ---------------------------------------------------------------------------
double OneEuroFilter::alpha(double cutoff, double dt) const noexcept {
    // τ = 1/(2π·fc)
    // τ/dt = 1/(2π·fc·dt)
    // α = 1/(1 + τ/dt) = 2π·fc·dt / (1 + 2π·fc·dt)
    double tau_over_dt = 1.0 / (2.0 * std::numbers::pi * cutoff * dt);
    double a = 1.0 / (1.0 + tau_over_dt);
    // Clamp: 1e-6 prevents denormals, 1.0 prevents overshoot
    if (a < 1e-6) a = 1e-6;
    if (a > 1.0)  a = 1.0;
    return a;
}

// ---------------------------------------------------------------------------
// filter — core single-axis 1€ filter
//
// Algorithm (per the CHI 2012 paper):
//   1. Estimate derivative:  dx = (raw - prevRaw) / dt
//   2. Low-pass derivative:  dxHat = α_d · dx + (1-α_d) · prevDx
//   3. Adaptive cutoff:      fc = minCutoff + β · |dxHat| · speedCoeff
//   4. Low-pass signal:      filtered = α(fc) · raw + (1-α(fc)) · prevFiltered
//
// NUMERICAL NOTE: When dt is very small (1ms at 1000Hz), α becomes very small
// for low cutoff frequencies. This is intentional — it provides heavy smoothing.
// For fc=1Hz, dt=1ms: α ≈ 0.0063, so each new sample contributes only 0.6%.
// ---------------------------------------------------------------------------
double OneEuroFilter::filter(double raw, double dt) noexcept {
    // Guard against zero/negative dt (first call, clock glitch, etc.)
    if (dt < m_minJitter) {
        dt = m_minJitter;
    }

    // First sample: bootstrap state, no filtering possible
    if (!m_initialized) {
        m_prevRaw      = raw;
        m_prevFiltered = raw;
        m_prevDx       = 0.0;
        m_initialized  = true;
        return raw;
    }

    // ── Step 1: Derivative estimation ──
    double dx = (raw - m_prevRaw) / dt;
    m_prevRaw = raw;

    // ── Step 2: Low-pass the derivative ──
    double ad    = alpha(m_dCutoff, dt);
    double dxHat = ad * dx + (1.0 - ad) * m_prevDx;
    m_prevDx     = dxHat;

    // ── Step 3: Adaptive cutoff ──
    double speed  = std::abs(dxHat) * m_speedCoeff;
    double cutoff = m_minCutoff + m_beta * speed;

    // ── Step 4: Low-pass the signal with adaptive cutoff ──
    double a        = alpha(cutoff, dt);
    double filtered = a * raw + (1.0 - a) * m_prevFiltered;
    m_prevFiltered  = filtered;

    return filtered;
}

// ---------------------------------------------------------------------------
// filterWithCutoff — filter using an externally-computed cutoff
//
// Used by OneEuroFilter2D::filterShared(): the 2D speed magnitude drives
// a single adaptive cutoff for both axes, while each axis maintains its
// own filter state (prevRaw, prevFiltered, prevDx).
// ---------------------------------------------------------------------------
double OneEuroFilter::filterWithCutoff(double raw, double dt,
                                        double externalCutoff) noexcept {
    if (dt < m_minJitter) dt = m_minJitter;

    if (!m_initialized) {
        m_prevRaw      = raw;
        m_prevFiltered = raw;
        m_prevDx       = 0.0;
        m_initialized  = true;
        return raw;
    }

    // Still track per-axis derivative for filter state consistency
    double dx     = (raw - m_prevRaw) / dt;
    m_prevRaw     = raw;
    double ad     = alpha(m_dCutoff, dt);
    double dxHat  = ad * dx + (1.0 - ad) * m_prevDx;
    m_prevDx      = dxHat;

    // Use the caller-provided cutoff instead of computing our own
    double a        = alpha(std::max(externalCutoff, 0.001), dt);
    double filtered = a * raw + (1.0 - a) * m_prevFiltered;
    m_prevFiltered  = filtered;

    return filtered;
}

// ---------------------------------------------------------------------------
// betaFromSeverity — exponential mapping of tremor level → β
//
// The mapping is exponential because tremor amplitude grows non-linearly
// with severity. A linear mapping would under-filter severe tremor and
// over-filter mild cases.
//
// Severity 0 → β=0.05  (barely any adaptation, just light smoothing)
// Severity 3 → β≈0.7   (moderate adaptation)
// Severity 5 → β≈3     (significant adaptation)
// Severity 7 → β≈15    (strong adaptation for severe tremor)
// Severity 10→ β≈80    (very aggressive, ~150ms lag on fast flicks)
// ---------------------------------------------------------------------------
double OneEuroFilter::betaFromSeverity(int tremorLevel) noexcept {
    double t = std::clamp(static_cast<double>(tremorLevel), 0.0, 10.0) / 10.0;
    // exp(0)   = 1     → 0.05
    // exp(6)   ≈ 403   → 20
    // exp(6.9) ≈ 992   → 50
    return 0.05 * std::exp(6.9 * t);
}

} // namespace aether
