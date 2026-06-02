#pragma once
#include <cmath>
#include <numbers>
#include <algorithm>
#include <chrono>

namespace aether::math {

// High-resolution timer — returns seconds as double
inline double now_seconds() {
    using clock = std::chrono::high_resolution_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

// Clamp value to [lo, hi]
template<typename T>
inline T clamp(T val, T lo, T hi) {
    return std::max(lo, std::min(hi, val));
}

// Linear interpolation
template<typename T>
inline T lerp(T a, T b, double t) {
    return a + (b - a) * t;
}

// Low-pass single-pole smoothing factor from cutoff (Hz) and sample period (s)
inline double alpha_from_cutoff(double cutoff_hz, double dt) {
    // alpha = 1 / (1 + tau/dt)  where tau = 1/(2*pi*fc)
    double tau = 1.0 / (2.0 * std::numbers::pi * cutoff_hz);
    return 1.0 / (1.0 + tau / dt);
}

// Euclidean distance
inline double distance(double dx, double dy) {
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace aether::math
