#pragma once
// ============================================================================
// EMAFilter.hpp — Exponential Moving Average (1-pole IIR Low-pass)
//
// Simplest possible smoothing filter.  Zero-adaptation, constant α.
//
// Transfer function:  y[n] = α·x[n] + (1-α)·y[n-1]
// Cutoff (-3dB):      fc ≈ α / (2π·dt)    for α ≪ 1
//
// USE CASE: Baseline comparison ("off" position on smoothing slider).
//           Also useful for testing — if EMA performs similarly to OneEuro,
//           the user likely doesn't need adaptive filtering.
//
// Latency: ~0.5ns per call (1 multiply-add + 1 load/store)
// ============================================================================

#include <algorithm>

namespace aether {

class EMAFilter {
public:
    explicit EMAFilter(double alpha = 0.6) noexcept
        : m_alpha(std::clamp(alpha, 0.0, 1.0))
    {}

    // ── Parameters ──
    void setAlpha(double a) noexcept { m_alpha = std::clamp(a, 0.0, 1.0); }
    double getAlpha() const noexcept { return m_alpha; }

    // ── State ──
    void reset() noexcept { m_initialized = false; }
    bool isInitialized() const noexcept { return m_initialized; }

    // ── Core: filter single value ──
    double filter(double raw) noexcept {
        if (!m_initialized) {
            m_prev = raw;
            m_initialized = true;
            return raw;
        }
        m_prev = m_alpha * raw + (1.0 - m_alpha) * m_prev;
        return m_prev;
    }

private:
    double m_alpha;
    double m_prev = 0.0;
    bool   m_initialized = false;
};

// ============================================================================
// EMAFilter2D — Two-axis EMA (independent per axis)
// ============================================================================
class EMAFilter2D {
public:
    struct Vec2 { double x, y; };

    explicit EMAFilter2D(double alpha = 0.6) noexcept
        : m_filterX(alpha), m_filterY(alpha) {}

    void setAlpha(double a) noexcept {
        m_filterX.setAlpha(a);
        m_filterY.setAlpha(a);
    }

    void reset() noexcept {
        m_filterX.reset();
        m_filterY.reset();
    }

    Vec2 filter(double dx, double dy) noexcept {
        return { m_filterX.filter(dx), m_filterY.filter(dy) };
    }

private:
    EMAFilter m_filterX;
    EMAFilter m_filterY;
};

} // namespace aether
