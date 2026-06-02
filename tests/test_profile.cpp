#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <filesystem>
#include <fstream>
#include <cmath>
#include "config/ProfileData.hpp"
#include "config/ProfileManager.hpp"

using namespace aether;
namespace fs = std::filesystem;

static const std::string TEST_PROFILE_DIR = "tests/tmp_profiles";

struct ProfileTestFixture {
    ProfileTestFixture() {
        fs::remove_all(TEST_PROFILE_DIR);
        fs::create_directories(TEST_PROFILE_DIR);
    }
    ~ProfileTestFixture() {
        fs::remove_all(TEST_PROFILE_DIR);
    }
};

// ============================================================================
// CRUD
// ============================================================================

TEST_CASE("ProfileManager creates blank profile", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);

    auto blank = pm.createBlank("test_user", "cs2");
    REQUIRE(blank.name == "test_user");
    REQUIRE(blank.game == "cs2");
    REQUIRE(blank.tremorSeverity == Catch::Approx(0.0));
    REQUIRE(blank.preferredDPI == 800);

    // Assign to active
    pm.active() = blank;
    REQUIRE(pm.active().name == "test_user");
}

TEST_CASE("ProfileManager save and load round-trip", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);

    // Create and populate
    auto& p = pm.active();
    p.name = "roundtrip_user";
    p.game = "apex";
    p.tremorSeverity = 4.5;
    p.overshootTendency = 3.0;
    p.fatigueRate = 1.2;
    p.reactionDelayMs = 50.0;
    p.tremorSeverityX = 5.0;
    p.tremorSeverityY = 3.0;
    p.preferredDPI = 1600;
    p.preferredSensitivity = 1.8;
    p.notes = "Test profile for round-trip verification";

    REQUIRE(pm.saveProfile());

    // Load into a new manager
    ProfileManager pm2(TEST_PROFILE_DIR);
    REQUIRE(pm2.loadProfile("roundtrip_user"));

    const auto& loaded = pm2.active();
    REQUIRE(loaded.name == "roundtrip_user");
    REQUIRE(loaded.game == "apex");
    REQUIRE(loaded.tremorSeverity == Catch::Approx(4.5));
    REQUIRE(loaded.overshootTendency == Catch::Approx(3.0));
    REQUIRE(loaded.fatigueRate == Catch::Approx(1.2));
    REQUIRE(loaded.reactionDelayMs == Catch::Approx(50.0));
    REQUIRE(loaded.tremorSeverityX == Catch::Approx(5.0));
    REQUIRE(loaded.tremorSeverityY == Catch::Approx(3.0));
    REQUIRE(loaded.preferredDPI == 1600);
    REQUIRE(loaded.preferredSensitivity == Catch::Approx(1.8));
    REQUIRE(loaded.notes == "Test profile for round-trip verification");
}

TEST_CASE("ProfileManager lists profiles", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);

    // Save a few profiles
    pm.active().name = "alice";
    pm.saveProfile();
    pm.active().name = "bob";
    pm.saveProfile();
    pm.active().name = "charlie";
    pm.saveProfile();

    auto list = pm.listProfiles();
    REQUIRE(list.size() == 3);
    REQUIRE(list[0] == "alice");
    REQUIRE(list[1] == "bob");
    REQUIRE(list[2] == "charlie");
}

TEST_CASE("ProfileManager delete removes file", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);

    pm.active().name = "to_delete";
    pm.saveProfile();
    REQUIRE(pm.listProfiles().size() == 1);

    REQUIRE(pm.deleteProfile("to_delete"));
    REQUIRE(pm.listProfiles().empty());
}

TEST_CASE("ProfileManager load nonexistent creates blank", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);

    bool loaded = pm.loadProfile("nonexistent");
    REQUIRE(!loaded);
    REQUIRE(pm.active().name == "nonexistent");
    // Should have default values
    REQUIRE(pm.active().tremorSeverity == Catch::Approx(0.0));
}

// ============================================================================
// Stats accumulation
// ============================================================================

TEST_CASE("ProfileManager accumulates samples", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);
    pm.startSession();

    // Push 200 samples
    for (int i = 0; i < 200; ++i) {
        DeltaSample s{};
        s.timestamp   = i * 0.001;  // 1ms intervals
        s.rawDx       = (i % 10) * 1.0;  // some variation
        s.rawDy       = (i % 7) * 1.0;
        s.filtDx      = s.rawDx * 0.8;
        s.filtDy      = s.rawDy * 0.8;
        s.rawSpeed    = std::sqrt(s.rawDx * s.rawDx + s.rawDy * s.rawDy) / 0.001;
        s.filtSpeed   = std::sqrt(s.filtDx * s.filtDx + s.filtDy * s.filtDy) / 0.001;
        s.dt          = 0.001;
        pm.pushSample(s);
    }

    REQUIRE(pm.sampleCount() == 200);
    REQUIRE(pm.totalSamples() == 200);
}

TEST_CASE("ProfileManager ring buffer evicts old samples", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);
    pm.startSession();

    // Push more than MAX_SAMPLES (6000)
    for (int i = 0; i < 7000; ++i) {
        DeltaSample s{};
        s.timestamp = i * 0.001;
        s.rawDx     = 1.0;
        s.rawDy     = 1.0;
        s.filtDx    = 0.8;
        s.filtDy    = 0.8;
        s.rawSpeed  = 1000.0;
        s.filtSpeed = 800.0;
        s.dt        = 0.001;
        pm.pushSample(s);
    }

    // Should cap at MAX_SAMPLES
    REQUIRE(pm.sampleCount() <= 6000);
    REQUIRE(pm.totalSamples() == 7000);  // Total still counts all
}

// ============================================================================
// Parameter recomputation
// ============================================================================

TEST_CASE("ProfileManager recomputeParameters updates derived values", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);
    pm.startSession();

    // Simulate a user with tremor: alternating small deltas at high freq
    for (int i = 0; i < 1000; ++i) {
        double t = i * 0.001;
        DeltaSample s{};
        s.timestamp = t;
        s.rawDx     = std::sin(t * 50.0) * 3.0;   // ~8 Hz oscillation
        s.rawDy     = std::sin(t * 50.0 + 1.0) * 2.0;
        s.filtDx    = std::sin(t * 50.0) * 1.0;    // Filtered: reduced amplitude
        s.filtDy    = std::sin(t * 50.0 + 1.0) * 0.7;
        s.rawSpeed  = std::sqrt(s.rawDx * s.rawDx + s.rawDy * s.rawDy) / 0.001;
        s.filtSpeed = std::sqrt(s.filtDx * s.filtDx + s.filtDy * s.filtDy) / 0.001;
        s.dt        = 0.001;
        pm.pushSample(s);
    }

    pm.recomputeParameters();

    const auto& p = pm.active();
    // Should have detected tremor frequency (~8 Hz)
    REQUIRE(p.tremorFreqHz > 0.0);
    // Filtered speed should be lower than raw speed
    REQUIRE(p.avgFilteredSpeed < p.avgRawSpeed);
    // Should have derived recommended parameters
    REQUIRE(p.recommendedBeta > 0.0);
    REQUIRE(p.recommendedMinCutoff > 0.0);
}

TEST_CASE("ProfileManager recomputeParameters handles empty buffer", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);

    // Should not crash with empty samples
    pm.recomputeParameters();
    REQUIRE(pm.active().tremorFreqHz == Catch::Approx(0.0));
}

// ============================================================================
// Session management
// ============================================================================

TEST_CASE("ProfileManager session start/end resets counters", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);

    pm.active().name = "session_test";
    pm.startSession();

    // Push some samples
    for (int i = 0; i < 50; ++i) {
        DeltaSample s{};
        s.timestamp = i * 0.001;
        s.rawDx = s.rawDy = s.filtDx = s.filtDy = 1.0;
        s.rawSpeed = s.filtSpeed = 100.0;
        s.dt = 0.001;
        pm.pushSample(s);
    }

    REQUIRE(pm.sampleCount() == 50);
    REQUIRE(pm.sessionDurationS() > 0.0);

    pm.endSession();

    // Should have saved to disk
    REQUIRE(fs::exists(TEST_PROFILE_DIR + "/session_test.json"));
}

// ============================================================================
// Estimator accuracy
// ============================================================================

TEST_CASE("estimateTremorFrequency detects known frequency", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);
    pm.startSession();

    // Generate a clean 6 Hz signal
    const double targetFreq = 6.0;
    const double dt = 0.001;
    for (int i = 0; i < 2000; ++i) {
        double t = i * dt;
        DeltaSample s{};
        s.timestamp = t;
        s.rawDx     = std::sin(2.0 * 3.14159 * targetFreq * t) * 5.0;
        s.rawDy     = 0.0;
        s.filtDx    = s.rawDx * 0.5;
        s.filtDy    = 0.0;
        s.rawSpeed  = std::abs(s.rawDx) / dt;
        s.filtSpeed = std::abs(s.filtDx) / dt;
        s.dt        = dt;
        pm.pushSample(s);
    }

    pm.recomputeParameters();

    // Should detect ~6 Hz (allow ±2 Hz error for zero-crossing method)
    double detected = pm.active().tremorFreqHz;
    REQUIRE(detected > 3.0);
    REQUIRE(detected < 10.0);
}

TEST_CASE("estimateOvershootRatio detects simulated overshoots", "[Profile]") {
    ProfileTestFixture fix;
    ProfileManager pm(TEST_PROFILE_DIR);
    pm.startSession();

    // Generate movements with overshoot pattern:
    // speed: 0 → peak → reverse → settle
    for (int flick = 0; flick < 20; ++flick) {
        double baseTime = flick * 0.2;
        // Acceleration
        for (int i = 0; i < 5; ++i) {
            DeltaSample s{};
            s.timestamp = baseTime + i * 0.001;
            s.rawDx    = 3.0 + i * 4.0;  // increasing speed
            s.rawDy    = 0.0;
            s.filtDx   = s.rawDx;
            s.filtDy   = 0.0;
            s.rawSpeed = std::abs(s.rawDx) / 0.001;
            s.filtSpeed = s.rawSpeed;
            s.dt       = 0.001;
            pm.pushSample(s);
        }
        // Overshoot + reversal
        for (int i = 0; i < 5; ++i) {
            DeltaSample s{};
            s.timestamp = baseTime + 0.005 + i * 0.001;
            s.rawDx    = 20.0 - i * 5.0;  // decelerating past target
            s.rawDy    = 0.0;
            s.filtDx   = s.rawDx;
            s.filtDy   = 0.0;
            s.rawSpeed = std::abs(s.rawDx) / 0.001;
            s.filtSpeed = s.rawSpeed;
            s.dt       = 0.001;
            pm.pushSample(s);
        }
    }

    pm.recomputeParameters();
    // Should detect some overshoots
    REQUIRE(pm.active().overshootRatio > 0.0);
}
