#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include "core/OneEuroFilter.hpp"
#include "core/EMAFilter.hpp"
#include "core/KalmanFilter.hpp"

using namespace aether;

// ============================================================================
// OneEuroFilter — 1D
// ============================================================================

TEST_CASE("OneEuroFilter initializes with defaults", "[OneEuro]") {
    OneEuroFilter f;
    REQUIRE(f.getMinCutoff() == Catch::Approx(1.2).margin(0.01));
    REQUIRE(f.getBeta() == Catch::Approx(0.0));
    REQUIRE(!f.isInitialized());
}

TEST_CASE("OneEuroFilter first sample passes through", "[OneEuro]") {
    OneEuroFilter f(1.0, 10.0);  // high beta
    double out = f.filter(50.0, 0.016);
    // First sample bootstraps, should equal input
    REQUIRE(out == Catch::Approx(50.0).margin(1.0));
}

TEST_CASE("OneEuroFilter converges on constant signal", "[OneEuro]") {
    OneEuroFilter f(1.0, 0.0, 1.0, 0.001, 1.0);
    // Feed constant value
    for (int i = 0; i < 100; ++i) {
        f.filter(100.0, 0.016);
    }
    double last = f.filter(100.0, 0.016);
    REQUIRE(last == Catch::Approx(100.0).margin(0.5));
}

TEST_CASE("OneEuroFilter attenuates high-frequency noise", "[OneEuro]") {
    OneEuroFilter f(1.0, 0.0, 1.0, 0.001, 1.0);  // beta=0 → fixed cutoff
    // Stabilize at 0
    for (int i = 0; i < 30; ++i) f.filter(0.0, 0.016);

    // Inject a spike
    double spike = f.filter(20.0, 0.016);
    double recovery = f.filter(0.0, 0.016);

    // Spike should be attenuated
    REQUIRE(std::abs(spike) < 20.0);
    // Recovery should be lower than spike
    REQUIRE(std::abs(recovery) < std::abs(spike));
}

TEST_CASE("OneEuroFilter higher beta = less lag on fast movement", "[OneEuro]") {
    OneEuroFilter lowBeta(1.0, 0.0);   // no adaptation
    OneEuroFilter highBeta(1.0, 50.0); // strong adaptation

    // Stabilize both
    for (int i = 0; i < 30; ++i) {
        lowBeta.filter(0.0, 0.016);
        highBeta.filter(0.0, 0.016);
    }

    // Simulate a flick: large delta
    double lowOut  = lowBeta.filter(100.0, 0.016);
    double highOut = highBeta.filter(100.0, 0.016);

    // High beta should pass more of the signal through (less lag)
    REQUIRE(highOut > lowOut);
}

TEST_CASE("OneEuroFilter betaFromSeverity maps correctly", "[OneEuro]") {
    // Severity 0 → near-zero beta
    REQUIRE(OneEuroFilter::betaFromSeverity(0) < 1.0);
    // Severity 5 → moderate
    double mid = OneEuroFilter::betaFromSeverity(5);
    REQUIRE(mid > 1.0);
    REQUIRE(mid < 50.0);
    // Severity 10 → large
    REQUIRE(OneEuroFilter::betaFromSeverity(10) > 50.0);
    // Monotonic
    REQUIRE(OneEuroFilter::betaFromSeverity(3) < OneEuroFilter::betaFromSeverity(7));
}

TEST_CASE("OneEuroFilter reset clears state", "[OneEuro]") {
    OneEuroFilter f(1.0, 10.0);
    f.filter(50.0, 0.016);
    f.filter(50.0, 0.016);
    REQUIRE(f.isInitialized());

    f.reset();
    REQUIRE(!f.isInitialized());

    // After reset, first sample bootstraps again
    double out = f.filter(10.0, 0.016);
    REQUIRE(out == Catch::Approx(10.0).margin(5.0));
}

TEST_CASE("OneEuroFilter parameter setters clamp values", "[OneEuro]") {
    OneEuroFilter f;
    f.setMinCutoff(-5.0);   // negative → clamped to 0.001
    REQUIRE(f.getMinCutoff() == Catch::Approx(0.001));
    f.setBeta(-10.0);        // negative → clamped to 0
    REQUIRE(f.getBeta() == Catch::Approx(0.0));
    f.setSpeedCoeff(0.0);    // zero → clamped to 0.01
    REQUIRE(f.getSpeedCoeff() == Catch::Approx(0.01));
}

// ============================================================================
// OneEuroFilter2D
// ============================================================================

TEST_CASE("OneEuroFilter2D filters both axes", "[OneEuro2D]") {
    OneEuroFilter2D f2d(1.0, 0.0);
    for (int i = 0; i < 30; ++i) f2d.filter(0.0, 0.0, 0.016);

    auto r = f2d.filter(50.0, 30.0, 0.016);
    // Both axes should be filtered (not raw)
    REQUIRE(std::abs(r.x) < 50.0);
    REQUIRE(std::abs(r.y) < 30.0);
}

TEST_CASE("OneEuroFilter2D SharedCutoff produces valid output", "[OneEuro2D]") {
    OneEuroFilter2D f2d(1.0, 5.0, 1.0, 1.0, CutoffStrategy::SharedCutoff);
    for (int i = 0; i < 30; ++i) f2d.filter(0.0, 0.0, 0.016);

    auto r = f2d.filter(80.0, 80.0, 0.016);  // 45° diagonal flick
    // Both axes should have similar response (shared cutoff)
    double ratio = r.x / std::max(r.y, 1e-9);
    REQUIRE(ratio == Catch::Approx(1.0).margin(0.3));  // Similar magnitude
}

TEST_CASE("OneEuroFilter2D strategy switch resets state", "[OneEuro2D]") {
    OneEuroFilter2D f2d(1.0, 0.0, 1.0, 1.0, CutoffStrategy::Independent);
    for (int i = 0; i < 30; ++i) f2d.filter(0.0, 0.0, 0.016);

    // Switch strategy
    f2d.setStrategy(CutoffStrategy::SharedCutoff);
    f2d.reset();

    auto r = f2d.filter(10.0, 10.0, 0.016);
    REQUIRE(r.x == Catch::Approx(r.y).margin(3.0));
}

// ============================================================================
// EMAFilter
// ============================================================================

TEST_CASE("EMAFilter alpha=1 is passthrough", "[EMA]") {
    EMAFilter f(1.0);
    REQUIRE(f.filter(42.0) == Catch::Approx(42.0));
    REQUIRE(f.filter(-10.0) == Catch::Approx(-10.0));
}

TEST_CASE("EMAFilter alpha=0 freezes at first value", "[EMA]") {
    EMAFilter f(0.0);
    double first = f.filter(42.0);
    // After bootstrap, alpha=0 means output = previous output (frozen)
    double second = f.filter(100.0);
    REQUIRE(first == Catch::Approx(42.0));
    REQUIRE(second == Catch::Approx(42.0));  // Frozen
}

TEST_CASE("EMAFilter smooths step input", "[EMA]") {
    EMAFilter f(0.5);
    f.filter(0.0);  // bootstrap
    double out = f.filter(10.0);
    // alpha=0.5, prev=0, raw=10 → 0.5*10 + 0.5*0 = 5
    REQUIRE(out == Catch::Approx(5.0).margin(0.01));
}

TEST_CASE("EMAFilter2D filters independently", "[EMA]") {
    EMAFilter2D f(0.5);
    auto r = f.filter(10.0, 20.0);
    REQUIRE(r.x == Catch::Approx(5.0).margin(0.01));
    REQUIRE(r.y == Catch::Approx(10.0).margin(0.01));
}

// ============================================================================
// KalmanFilter
// ============================================================================

TEST_CASE("KalmanFilter initializes with defaults", "[Kalman]") {
    KalmanFilter k;
    REQUIRE(k.getProcessNoise() == Catch::Approx(0.01));
    REQUIRE(k.getMeasurementNoise() == Catch::Approx(0.1));
}

TEST_CASE("KalmanFilter converges to constant signal", "[Kalman]") {
    KalmanFilter k(0.01, 0.1);
    // Feed constant value many times
    double last = 0.0;
    for (int i = 0; i < 200; ++i) {
        last = k.filter(50.0);
    }
    // Should converge close to 50
    REQUIRE(last == Catch::Approx(50.0).margin(2.0));
}

TEST_CASE("KalmanFilter high Q = more responsive", "[Kalman]") {
    KalmanFilter lowQ(0.001, 0.1);   // trusts model more
    KalmanFilter highQ(10.0, 0.1);   // trusts measurements more

    for (int i = 0; i < 50; ++i) {
        lowQ.filter(0.0);
        highQ.filter(0.0);
    }

    // Step change
    double lowResp  = lowQ.filter(100.0);
    double highResp = highQ.filter(100.0);

    // High Q (more responsive) should move closer to 100
    REQUIRE(highResp > lowResp);
}

TEST_CASE("KalmanFilter high R = more smoothing", "[Kalman]") {
    KalmanFilter lowR(0.01, 0.01);   // trusts measurements
    KalmanFilter highR(0.01, 10.0);   // trusts model (more smoothing)

    for (int i = 0; i < 50; ++i) {
        lowR.filter(0.0);
        highR.filter(0.0);
    }

    // Inject noise
    double lowResp  = lowR.filter(50.0);
    double highResp = highR.filter(50.0);

    // Higher R should be more conservative (less response to new measurement)
    REQUIRE(highResp < lowResp);
}

TEST_CASE("KalmanFilter reset restores initial uncertainty", "[Kalman]") {
    KalmanFilter k(0.01, 0.1, 100.0);  // High initial uncertainty
    for (int i = 0; i < 100; ++i) k.filter(0.0);

    k.reset();
    // After reset, first measurement should have greater influence
    double postReset = k.filter(50.0);
    // With high P0, first measurement strongly pulls estimate
    REQUIRE(postReset > 25.0);  // More than halfway to 50
}
