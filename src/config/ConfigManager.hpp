#pragma once
// ============================================================================
// ConfigManager.hpp — JSON File ↔ GlobalAppConfig persistence layer
//
// Features:
//   - Load / Save JSON config files
//   - Hot reload: detect file changes on disk, auto-reload, notify listeners
//   - Default config generation (first-run experience)
//   - Thread-safe parameter read (const ref) + write (save serialized)
//
// Usage:
//   ConfigManager cfg("assets/default_config.json");
//   cfg.load();                              // initial load
//   cfg.onChange([](auto& c) { apply(c); }); // register listener
//   ...
//   cfg.reloadIfChanged();                   // call periodically (e.g. in GUI loop)
//   cfg.save();                              // explicit save
// ============================================================================

#include "GlobalAppConfig.hpp"
#include <string>
#include <functional>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <atomic>

namespace aether {

class ConfigManager {
public:
    // ── Callback type: invoked on hot reload ──────────────────────────
    // Parameter: const reference to new config (read-only from callback)
    using ChangeCallback = std::function<void(const GlobalAppConfig& newConfig)>;

    // ── Construction ───────────────────────────────────────────────────
    // path: relative or absolute path to JSON config file
    explicit ConfigManager(const std::string& path = "assets/default_config.json");
    ~ConfigManager();

    // Non-copyable (owns file handle state)
    ConfigManager(const ConfigManager&)            = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // ── Persistence ────────────────────────────────────────────────────
    // Load from disk. Returns false if file missing or parse error.
    // On first run (file missing), generates default config and saves it.
    bool load();

    // Save current config to disk (atomic write: temp + rename)
    bool save();

    // Generate a fresh default config and save it
    bool generateDefault();

    // ── Hot reload ─────────────────────────────────────────────────────
    // Check if the file has been modified since last load/reload.
    // If changed: reload, invoke onChange callbacks, return true.
    // Safe to call every frame (just a stat() + timestamp compare).
    bool reloadIfChanged();

    // Register a callback invoked after successful load or hot reload.
    // Use this to push new parameters to InputProcessor, GUI, etc.
    void onChange(ChangeCallback cb);

    // ── Config access (thread-safe reads) ─────────────────────────────
    const GlobalAppConfig& config() const { return m_config; }

    // Mutable access — caller MUST call save() after modifications.
    // Not thread-safe for write; intended for GUI-driven edits.
    GlobalAppConfig& configMutable() { return m_config; }

    // ── Convenience getters (read-only, thread-safe) ──────────────────
    bool    isEnabled()  const { return m_config.global.enabled; }
    int32_t hotkey()     const { return m_config.global.hotkey; }
    int32_t hotkeyAlt()  const { return m_config.global.hotkeyAlt; }
    const FilterSettings&    filter()   const { return m_config.filter; }
    const AdvancedSettings&  advanced() const { return m_config.advanced; }
    const ProfileSettings&   profiles() const { return m_config.profiles; }

    // ── Single-field setters (GUI convenience) ─────────────────────────
    void setEnabled(bool v);
    void setFilterType(const std::string& type);
    void setOneEuroParams(double minCutoff, double beta, double dCutoff, double speedCoeff);
    void setEMAParams(double alpha);
    void setKalmanParams(double q, double r);

    // ── File path ──────────────────────────────────────────────────────
    const std::string& path() const { return m_path; }

private:
    // Atomic write: write to .tmp, then rename over target
    bool atomicSave(const std::string& content);

    std::string        m_path;
    GlobalAppConfig    m_config;
    ChangeCallback     m_onChange;

    // Hot reload tracking
    std::filesystem::file_time_type m_lastWriteTime;
    mutable std::mutex m_mutex;       // protects m_config during reload
};

} // namespace aether
