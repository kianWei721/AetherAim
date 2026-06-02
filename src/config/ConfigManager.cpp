// ============================================================================
// ConfigManager.cpp — Implementation
// ============================================================================

#include "ConfigManager.hpp"
#include "utils/Logger.hpp"
#include <fstream>
#include <sstream>
#include <chrono>

namespace aether {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
ConfigManager::ConfigManager(const std::string& path)
    : m_path(path)
{}

ConfigManager::~ConfigManager() {
    // Save on destruction (best-effort, don't throw)
    try { save(); } catch (...) {}
}

// ---------------------------------------------------------------------------
// load — read JSON from disk, gracefully handle missing/corrupt files
// ---------------------------------------------------------------------------
bool ConfigManager::load() {
    std::lock_guard lock(m_mutex);

    std::ifstream f(m_path);
    if (!f.is_open()) {
        LOG_WARN("Config file not found: '{}'. Generating defaults...", m_path);
        generateDefault();
        return true;  // Defaults are valid
    }

    try {
        nlohmann::json j = nlohmann::json::parse(f);
        m_config = j.get<GlobalAppConfig>();

        // Record file timestamp for hot reload detection
        std::error_code ec;
        m_lastWriteTime = std::filesystem::last_write_time(m_path, ec);

        LOG_INFO("Config loaded from '{}' (v{})", m_path, m_config.schemaVersion);
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Config parse error in '{}': {}", m_path, e.what());
        LOG_WARN("Falling back to defaults");
        generateDefault();
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Config load error: {}", e.what());
        return false;
    }

    // Notify listeners
    if (m_onChange) {
        m_onChange(m_config);
    }

    return true;
}

// ---------------------------------------------------------------------------
// save — write current config to disk
// ---------------------------------------------------------------------------
bool ConfigManager::save() {
    std::lock_guard lock(m_mutex);

    try {
        nlohmann::json j = m_config;
        std::string content = j.dump(4);  // 4-space indent, human-readable

        if (!atomicSave(content)) {
            LOG_ERROR("Failed to write config to '{}'", m_path);
            return false;
        }

        // Update timestamp after save (so hot reload doesn't re-trigger)
        std::error_code ec;
        m_lastWriteTime = std::filesystem::last_write_time(m_path, ec);

        LOG_DEBUG("Config saved to '{}' ({} bytes)", m_path, content.size());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Config save error: {}", e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// generateDefault — populate with sensible defaults and persist
// ---------------------------------------------------------------------------
bool ConfigManager::generateDefault() {
    m_config = GlobalAppConfig{};  // Reset to compiler defaults

    // Override with curated defaults for first-run experience
    m_config.schemaVersion                    = "1.0";
    m_config.global.enabled                   = false;
    m_config.global.hotkey                    = 0x90;   // NumLock
    m_config.global.hotkeyAlt                 = 0x77;   // F8
    m_config.global.startMinimized            = false;
    m_config.global.runOnStartup              = false;

    m_config.filter.type                      = "one_euro";
    m_config.filter.oneEuro.minCutoff         = 1.2;
    m_config.filter.oneEuro.beta              = 0.0;
    m_config.filter.oneEuro.dCutoff           = 1.0;
    m_config.filter.oneEuro.speedCoeff        = 1.0;
    m_config.filter.oneEuro.minJitter         = 0.001;
    m_config.filter.ema.alpha                 = 0.6;
    m_config.filter.kalman.processNoise       = 0.01;
    m_config.filter.kalman.measurementNoise   = 0.1;
    m_config.filter.kalman.initialError       = 1.0;

    m_config.advanced.deadzonePx              = 0;
    m_config.advanced.maxDeltaPx              = 100;
    m_config.advanced.pollingRateHz           = 1000;
    m_config.advanced.subPixelAccum           = true;

    m_config.profiles.activeProfile           = "default";
    m_config.profiles.profileDir              = "./profiles/";
    m_config.profiles.autoSave                = true;
    m_config.profiles.autoSaveIntervalS       = 60;

    m_config.gui.windowX                      = 100;
    m_config.gui.windowY                      = 100;
    m_config.gui.windowW                      = 900;
    m_config.gui.windowH                      = 700;
    m_config.gui.showRealtimePlot             = true;
    m_config.gui.showRadarChart               = true;
    m_config.gui.plotHistoryS                 = 3.0f;

    LOG_INFO("Default config generated");
    // Persist immediately
    return save();
}

// ---------------------------------------------------------------------------
// reloadIfChanged — hot reload detection
//
// Uses std::filesystem::last_write_time which maps to GetFileTime on Windows.
// Resolution is ~100ns (NTFS), more than enough for config file changes.
// Called from GUI main loop (~60 Hz) — overhead is negligible.
// ---------------------------------------------------------------------------
bool ConfigManager::reloadIfChanged() {
    std::error_code ec;
    auto currentTime = std::filesystem::last_write_time(m_path, ec);

    if (ec) {
        // File doesn't exist or can't be accessed — not an error
        return false;
    }

    if (currentTime == m_lastWriteTime) {
        return false;  // No change
    }

    LOG_INFO("Config file changed on disk — hot reloading...");

    std::lock_guard lock(m_mutex);

    std::ifstream f(m_path);
    if (!f.is_open()) {
        LOG_WARN("Hot reload: cannot open '{}'", m_path);
        return false;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(f);
        GlobalAppConfig newConfig = j.get<GlobalAppConfig>();

        // Preserve certain runtime state that shouldn't be overwritten
        // by hot reload (e.g., if GUI changed enabled state)
        // For now: full replacement. GUI can re-apply overrides.

        m_config = std::move(newConfig);
        m_lastWriteTime = currentTime;

        LOG_INFO("Hot reload complete");
    } catch (const std::exception& e) {
        LOG_ERROR("Hot reload parse error: {} — keeping current config", e.what());
        m_lastWriteTime = currentTime;  // Don't retry the broken file
        return false;
    }

    // Notify listeners
    if (m_onChange) {
        m_onChange(m_config);
    }

    return true;
}

// ---------------------------------------------------------------------------
// onChange — register a reload callback
// ---------------------------------------------------------------------------
void ConfigManager::onChange(ChangeCallback cb) {
    // Wrapping to invoke under lock if needed, but callbacks should be fast.
    // We store the callback and invoke it from load() / reloadIfChanged()
    // while already holding m_mutex. Callers should NOT call back into
    // ConfigManager from their callback (deadlock risk).
    m_onChange = std::move(cb);
}

// ---------------------------------------------------------------------------
// Single-field setters (GUI convenience)
// ---------------------------------------------------------------------------
void ConfigManager::setEnabled(bool v) {
    m_config.global.enabled = v;
}

void ConfigManager::setFilterType(const std::string& type) {
    m_config.filter.type = type;
}

void ConfigManager::setOneEuroParams(double minCutoff, double beta,
                                      double dCutoff, double speedCoeff) {
    auto& oe = m_config.filter.oneEuro;
    oe.minCutoff  = minCutoff;
    oe.beta       = beta;
    oe.dCutoff    = dCutoff;
    oe.speedCoeff = speedCoeff;
}

void ConfigManager::setEMAParams(double alpha) {
    m_config.filter.ema.alpha = alpha;
}

void ConfigManager::setKalmanParams(double q, double r) {
    m_config.filter.kalman.processNoise     = q;
    m_config.filter.kalman.measurementNoise = r;
}

// ---------------------------------------------------------------------------
// atomicSave — write to .tmp, rename over target
//
// Prevents corruption if the process crashes mid-write.
// ---------------------------------------------------------------------------
bool ConfigManager::atomicSave(const std::string& content) {
    std::string tmpPath = m_path + ".tmp";

    // Write to temp file
    std::ofstream tmp(tmpPath, std::ios::binary | std::ios::trunc);
    if (!tmp.is_open()) {
        LOG_ERROR("Cannot open temp file for writing: '{}'", tmpPath);
        return false;
    }
    tmp << content;
    if (!tmp.good()) {
        LOG_ERROR("Write to temp file failed: '{}'", tmpPath);
        tmp.close();
        std::filesystem::remove(tmpPath);
        return false;
    }
    tmp.close();

    // Atomic rename
    std::error_code ec;
    std::filesystem::rename(tmpPath, m_path, ec);
    if (ec) {
        LOG_ERROR("Rename '{}' → '{}' failed: {}", tmpPath, m_path, ec.message());
        std::filesystem::remove(tmpPath);
        return false;
    }

    return true;
}

} // namespace aether
