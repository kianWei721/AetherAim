#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <fstream>
#include <filesystem>
#include "config/GlobalAppConfig.hpp"
#include "config/ConfigManager.hpp"

using namespace aether;
namespace fs = std::filesystem;

static const std::string TEST_CONFIG_PATH = "tests/tmp_test_config.json";

// Clean up before/after
struct ConfigTestFixture {
    ConfigTestFixture() {
        fs::remove(TEST_CONFIG_PATH);
        fs::remove(TEST_CONFIG_PATH + ".tmp");
    }
    ~ConfigTestFixture() {
        fs::remove(TEST_CONFIG_PATH);
        fs::remove(TEST_CONFIG_PATH + ".tmp");
    }
};

// ============================================================================
// Default generation
// ============================================================================

TEST_CASE("ConfigManager generates defaults when file missing", "[Config]") {
    ConfigTestFixture fix;
    fs::remove(TEST_CONFIG_PATH);

    ConfigManager mgr(TEST_CONFIG_PATH);
    bool ok = mgr.load();  // File missing → generates defaults

    REQUIRE(ok);
    // Verify default values
    REQUIRE(mgr.config().schemaVersion == "1.0");
    REQUIRE(mgr.config().global.hotkey == 0x90);       // VK_NUMLOCK
    REQUIRE(mgr.config().global.hotkeyAlt == 0x77);     // VK_F8
    REQUIRE(mgr.config().filter.type == "one_euro");
    REQUIRE(mgr.config().filter.oneEuro.minCutoff == Catch::Approx(1.2));
    REQUIRE(mgr.config().filter.oneEuro.beta == Catch::Approx(0.0));
    REQUIRE(mgr.config().profiles.activeProfile == "default");
    REQUIRE(mgr.config().advanced.deadzonePx == 0);
    REQUIRE(mgr.config().advanced.maxDeltaPx == 100);

    // File should have been created
    REQUIRE(fs::exists(TEST_CONFIG_PATH));
}

// ============================================================================
// Save → Load round-trip
// ============================================================================

TEST_CASE("ConfigManager save-load round-trip preserves values", "[Config]") {
    ConfigTestFixture fix;

    // Create config with non-default values
    ConfigManager mgr(TEST_CONFIG_PATH);
    mgr.generateDefault();

    auto& cfg = mgr.configMutable();
    cfg.filter.type = "kalman";
    cfg.filter.oneEuro.minCutoff = 2.5;
    cfg.filter.oneEuro.beta = 42.0;
    cfg.filter.ema.alpha = 0.3;
    cfg.advanced.deadzonePx = 5;
    cfg.profiles.activeProfile = "test_user";
    cfg.global.hotkey = 0x70;  // F1

    REQUIRE(mgr.save());

    // Load into a new manager
    ConfigManager mgr2(TEST_CONFIG_PATH);
    REQUIRE(mgr2.load());

    const auto& cfg2 = mgr2.config();
    REQUIRE(cfg2.filter.type == "kalman");
    REQUIRE(cfg2.filter.oneEuro.minCutoff == Catch::Approx(2.5));
    REQUIRE(cfg2.filter.oneEuro.beta == Catch::Approx(42.0));
    REQUIRE(cfg2.filter.ema.alpha == Catch::Approx(0.3));
    REQUIRE(cfg2.advanced.deadzonePx == 5);
    REQUIRE(cfg2.profiles.activeProfile == "test_user");
    REQUIRE(cfg2.global.hotkey == 0x70);
}

// ============================================================================
// Setters
// ============================================================================

TEST_CASE("ConfigManager single-field setters update config", "[Config]") {
    ConfigTestFixture fix;
    ConfigManager mgr(TEST_CONFIG_PATH);
    mgr.generateDefault();

    mgr.setEnabled(true);
    REQUIRE(mgr.isEnabled() == true);

    mgr.setFilterType("ema");
    REQUIRE(mgr.config().filter.type == "ema");

    mgr.setOneEuroParams(3.0, 20.0, 2.0, 1.5);
    REQUIRE(mgr.config().filter.oneEuro.minCutoff == Catch::Approx(3.0));
    REQUIRE(mgr.config().filter.oneEuro.beta == Catch::Approx(20.0));

    mgr.setEMAParams(0.75);
    REQUIRE(mgr.config().filter.ema.alpha == Catch::Approx(0.75));

    mgr.setKalmanParams(0.05, 0.5);
    REQUIRE(mgr.config().filter.kalman.processNoise == Catch::Approx(0.05));
    REQUIRE(mgr.config().filter.kalman.measurementNoise == Catch::Approx(0.5));
}

// ============================================================================
// Hot reload
// ============================================================================

TEST_CASE("ConfigManager reloadIfChanged detects file modification", "[Config]") {
    ConfigTestFixture fix;

    ConfigManager mgr(TEST_CONFIG_PATH);
    mgr.generateDefault();

    // No change → no reload
    REQUIRE(!mgr.reloadIfChanged());

    // Modify file externally
    {
        std::ofstream f(TEST_CONFIG_PATH);
        nlohmann::json j;
        j["schemaVersion"] = "1.0";
        j["global"] = { {"enabled", true}, {"hotkey", 0x70}, {"hotkeyAlt", 0x71},
                        {"startMinimized", false}, {"runOnStartup", false} };
        j["filter"] = { {"type", "ema"},
            {"oneEuro", {{"minCutoff", 3.0}, {"beta", 10.0}, {"dCutoff", 1.0},
                         {"speedCoeff", 1.0}, {"minJitter", 0.001}}},
            {"ema", {{"alpha", 0.8}}},
            {"kalman", {{"processNoise", 0.01}, {"measurementNoise", 0.1},
                        {"initialError", 1.0}}} };
        j["advanced"] = { {"deadzonePx", 3}, {"maxDeltaPx", 150},
                          {"pollingRateHz", 1000}, {"subPixelAccum", true} };
        j["profiles"] = { {"activeProfile", "hot_reload_test"},
                          {"profileDir", "./profiles/"}, {"autoSave", true},
                          {"autoSaveIntervalS", 60} };
        j["gui"] = { {"windowX", 0}, {"windowY", 0}, {"windowW", 900},
                     {"windowH", 700}, {"showRealtimePlot", true},
                     {"showRadarChart", true}, {"plotHistoryS", 3.0} };
        f << j.dump(4);
    }

    // Should detect change and reload
    int changeCount = 0;
    mgr.onChange([&](const GlobalAppConfig&) { changeCount++; });
    REQUIRE(mgr.reloadIfChanged());
    REQUIRE(changeCount == 1);

    // Verify reloaded values
    REQUIRE(mgr.config().filter.type == "ema");
    REQUIRE(mgr.config().filter.oneEuro.minCutoff == Catch::Approx(3.0));
    REQUIRE(mgr.config().filter.ema.alpha == Catch::Approx(0.8));
    REQUIRE(mgr.config().advanced.deadzonePx == 3);
    REQUIRE(mgr.config().profiles.activeProfile == "hot_reload_test");
}

// ============================================================================
// Atomic save — no corruption on crash (simulated)
// ============================================================================

TEST_CASE("ConfigManager atomic save leaves no .tmp residue", "[Config]") {
    ConfigTestFixture fix;

    ConfigManager mgr(TEST_CONFIG_PATH);
    mgr.generateDefault();
    mgr.save();

    // No .tmp file should remain
    REQUIRE(!fs::exists(TEST_CONFIG_PATH + ".tmp"));
    // Original file should exist and be valid JSON
    std::ifstream f(TEST_CONFIG_PATH);
    REQUIRE(f.is_open());
    auto j = nlohmann::json::parse(f);
    REQUIRE(j["schemaVersion"] == "1.0");
}

// ============================================================================
// JSON ↔ GlobalAppConfig direct serialization
// ============================================================================

TEST_CASE("GlobalAppConfig NLOHMANN macros work correctly", "[Config]") {
    GlobalAppConfig cfg;

    nlohmann::json j = cfg;
    auto roundTripped = j.get<GlobalAppConfig>();

    REQUIRE(roundTripped.schemaVersion == cfg.schemaVersion);
    REQUIRE(roundTripped.filter.type == cfg.filter.type);
    REQUIRE(roundTripped.filter.oneEuro.minCutoff ==
            Catch::Approx(cfg.filter.oneEuro.minCutoff));
}
