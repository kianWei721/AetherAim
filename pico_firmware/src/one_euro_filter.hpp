#pragma once
// ============================================================================
// OneEuro Filter — Embedded (float) Edition
//
// Adapted from AetherAim core for microcontrollers without double-precision
// FPU (Cortex-M0+, RP2040). Uses single-precision float throughout.
//
// Performance on RP2040 @ 133MHz (soft-float):
//   ~800 cycles per filter() call ≈ 6 μs  (well under 1ms budget at 1000Hz)
// ============================================================================

#include <cmath>
#include <algorithm>

namespace aether {

class OneEuroFilter {
public:
    OneEuroFilter(float minCutoff  = 1.2f,
                  float beta       = 0.0f,
                  float dCutoff    = 1.0f,
                  float minJitter  = 0.001f,
                  float speedCoeff = 1.0f)
        : m_minCutoff(std::max(minCutoff, 0.001f))
        , m_beta(std::max(beta, 0.0f))
        , m_dCutoff(std::max(dCutoff, 0.001f))
        , m_minJitter(std::max(minJitter, 1e-6f))
        , m_speedCoeff(std::max(speedCoeff, 0.01f))
    {}

    void reset() {
        m_initialized  = false;
        m_prevRaw      = 0.0f;
        m_prevFiltered = 0.0f;
        m_prevDx       = 0.0f;
    }

    float filter(float raw, float dt) {
        if (dt < m_minJitter) dt = m_minJitter;

        if (!m_initialized) {
            m_prevRaw      = raw;
            m_prevFiltered = raw;
            m_prevDx       = 0.0f;
            m_initialized  = true;
            return raw;
        }

        // Step 1: Derivative
        float dx = (raw - m_prevRaw) / dt;
        m_prevRaw = raw;

        // Step 2: Low-pass derivative
        float tau_d  = 1.0f / (2.0f * 3.14159265f * m_dCutoff);
        float ad     = 1.0f / (1.0f + tau_d / dt);
        float dxHat  = ad * dx + (1.0f - ad) * m_prevDx;
        m_prevDx     = dxHat;

        // Step 3: Adaptive cutoff
        float speed  = std::abs(dxHat) * m_speedCoeff;
        float cutoff = m_minCutoff + m_beta * speed;

        // Step 4: Low-pass signal
        float tau = 1.0f / (2.0f * 3.14159265f * std::max(cutoff, 0.001f));
        float a   = 1.0f / (1.0f + tau / dt);
        float filtered = a * raw + (1.0f - a) * m_prevFiltered;
        m_prevFiltered = filtered;

        return filtered;
    }

    // Parameter accessors
    float getMinCutoff()  const { return m_minCutoff; }
    float getBeta()       const { return m_beta; }
    void  setMinCutoff(float v)  { m_minCutoff  = std::max(v, 0.001f); }
    void  setBeta(float v)       { m_beta       = std::max(v, 0.0f); }
    void  setDCutoff(float v)    { m_dCutoff    = std::max(v, 0.001f); }
    void  setSpeedCoeff(float v) { m_speedCoeff = std::max(v, 0.01f); }

    void  setParams(float fc, float b, float dc, float sc) {
        setMinCutoff(fc); setBeta(b); setDCutoff(dc); setSpeedCoeff(sc);
    }

private:
    float m_minCutoff;
    float m_beta;
    float m_dCutoff;
    float m_minJitter;
    float m_speedCoeff;
    float m_prevRaw      = 0.0f;
    float m_prevFiltered = 0.0f;
    float m_prevDx       = 0.0f;
    bool  m_initialized  = false;
};

// ============================================================================
// OneEuroFilter2D — two-axis filter for mouse delta
// ============================================================================
class OneEuroFilter2D {
public:
    struct Vec2 { float x, y; };

    OneEuroFilter2D(float minCutoff  = 1.2f,
                    float beta       = 0.0f,
                    float dCutoff    = 1.0f,
                    float speedCoeff = 1.0f)
        : m_filterX(minCutoff, beta, dCutoff, 0.001f, speedCoeff)
        , m_filterY(minCutoff, beta, dCutoff, 0.001f, speedCoeff)
    {}

    void reset() { m_filterX.reset(); m_filterY.reset(); }

    Vec2 filter(float dx, float dy, float dt) {
        return { m_filterX.filter(dx, dt), m_filterY.filter(dy, dt) };
    }

    void setParams(float fc, float b, float dc, float sc) {
        m_filterX.setParams(fc, b, dc, sc);
        m_filterY.setParams(fc, b, dc, sc);
    }

private:
    OneEuroFilter m_filterX;
    OneEuroFilter m_filterY;
};

} // namespace aether
