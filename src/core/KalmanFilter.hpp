#pragma once
// ============================================================================
// KalmanFilter.hpp — 1D Linear Kalman Filter for Mouse Delta Smoothing
//
// State model:  x[k] = x[k-1] + w_k     (constant position + process noise)
// Measurement:  z[k] = x[k]   + v_k     (direct measurement + sensor noise)
//
// This is the simplest possible Kalman — a 1D steady-state estimator.
// It converges to an EMA-like behavior where the Kalman gain K acts as
// a data-adaptive α that decreases as the filter becomes more confident.
//
// ADVANTAGE over EMA: self-tuning — K starts high (trust measurements),
// decreases as P converges (trust model more). Good when tremor
// characteristics change slowly over a gaming session.
//
// DISADVANTAGE: no speed adaptation (unlike OneEuro).
//               K converges to a constant value determined by Q/R ratio.
//
// Latency: ~1ns per call (3 multiplies + 2 adds, no divides in steady state)
// ============================================================================

#include <algorithm>

namespace aether {

class KalmanFilter {
public:
    // processNoise (Q):      how much the true value changes per step.
    //                         Higher = more responsive, less smoothing.
    // measurementNoise (R):  how noisy the measurements are.
    //                         Higher = more smoothing, more lag.
    // initialError (P0):     initial uncertainty. High = fast convergence.
    explicit KalmanFilter(double processNoise     = 0.01,
                          double measurementNoise = 0.1,
                          double initialError     = 1.0) noexcept
        : m_q(std::max(processNoise, 1e-9))
        , m_r(std::max(measurementNoise, 1e-9))
        , m_p0(initialError)
        , m_p(initialError)
        , m_x(0.0)
    {}

    // ── Parameters ──
    void setProcessNoise(double q)     noexcept { m_q = std::max(q, 1e-9); }
    void setMeasurementNoise(double r) noexcept { m_r = std::max(r, 1e-9); }
    void setInitialError(double p0)    noexcept { m_p0 = p0; }

    double getProcessNoise()     const noexcept { return m_q; }
    double getMeasurementNoise() const noexcept { return m_r; }

    // ── State ──
    void reset() noexcept {
        m_initialized = false;
        m_x = 0.0;
        m_p = m_p0;   // Reset uncertainty
    }

    // ── Core filter ──
    double filter(double raw) noexcept {
        if (!m_initialized) {
            m_x = raw;
            m_p = m_p0;
            m_initialized = true;
            return raw;
        }

        // Predict: x̂⁻ = x̂        (constant model, no control input)
        //          P⁻  = P + Q
        m_p = m_p + m_q;

        // Update:  K   = P⁻ / (P⁻ + R)
        //          x̂   = x̂⁻ + K·(z - x̂⁻)
        //          P   = (1 - K)·P⁻
        //
        // Optimized: compute K inline, reuse P⁻
        double k = m_p / (m_p + m_r);       // Kalman gain
        m_x = m_x + k * (raw - m_x);        // State update
        m_p = (1.0 - k) * m_p;              // Covariance update

        return m_x;
    }

private:
    double m_q;           // Process noise covariance
    double m_r;           // Measurement noise covariance
    double m_p0;          // Initial error covariance (for reset)
    double m_p;           // Current error covariance
    double m_x;           // State estimate
    bool   m_initialized = false;
};

// ============================================================================
// KalmanFilter2D — per-axis Kalman (independent)
// ============================================================================
class KalmanFilter2D {
public:
    struct Vec2 { double x, y; };

    KalmanFilter2D(double q = 0.01, double r = 0.1, double p0 = 1.0) noexcept
        : m_filterX(q, r, p0), m_filterY(q, r, p0) {}

    void reset() noexcept { m_filterX.reset(); m_filterY.reset(); }

    Vec2 filter(double dx, double dy) noexcept {
        return { m_filterX.filter(dx), m_filterY.filter(dy) };
    }

    void setParams(double q, double r) noexcept {
        m_filterX.setProcessNoise(q);
        m_filterX.setMeasurementNoise(r);
        m_filterY.setProcessNoise(q);
        m_filterY.setMeasurementNoise(r);
    }

    KalmanFilter& filterX() noexcept { return m_filterX; }
    KalmanFilter& filterY() noexcept { return m_filterY; }

private:
    KalmanFilter m_filterX;
    KalmanFilter m_filterY;
};

} // namespace aether
