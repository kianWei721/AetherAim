#pragma once
// ============================================================================
// ProfileManager.hpp — Multi-User Profile Lifecycle Manager
//
// Responsibilities:
//   - CRUD: Create, Read, Update, Delete user profiles (JSON files)
//   - Session: Accumulate DeltaSamples, recompute derived parameters
//   - Synchronization: Push profile-derived params → InputProcessor
//
// Thread Safety:
//   - Profile I/O (load/save/delete): GUI thread only (not thread-safe)
//   - pushSample(): called from hook thread via callback.
//                   Uses mutex-protected ring buffer.
//   - recomputeParameters(): called from GUI thread (reads buffer under lock)
//   - active() accessor: returns const ref (caller must ensure no concurrent mutation)
//
// Sample flow:
//   Hook thread:  pushSample(sample)  ← fast, lock-held push_back
//   GUI thread:   recomputeParameters() → reads samples, updates active profile
//   GUI thread:   saveProfile() → persists to JSON
// ============================================================================

#include "ProfileData.hpp"
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <chrono>

namespace aether {

// Forward declaration
class InputProcessor;

class ProfileManager {
public:
    explicit ProfileManager(const std::string& profileDir = "./profiles/");
    ~ProfileManager();

    // Non-copyable
    ProfileManager(const ProfileManager&)            = delete;
    ProfileManager& operator=(const ProfileManager&) = delete;

    // ── CRUD ───────────────────────────────────────────────────────────
    // Load a profile by name (reads <profileDir>/<name>.json)
    bool loadProfile(const std::string& name);

    // Save current active profile to disk
    bool saveProfile();
    bool saveProfileAs(const std::string& name);

    // List all available profiles in profileDir
    std::vector<std::string> listProfiles() const;

    // Delete a profile file
    bool deleteProfile(const std::string& name);

    // Create a new blank profile for a given game
    UserAbilityProfile createBlank(const std::string& name, const std::string& game);

    // ── Active Profile Access ──────────────────────────────────────────
    UserAbilityProfile&       active()       { return m_active; }
    const UserAbilityProfile& active() const { return m_active; }

    std::string activeName() const { return m_active.name; }

    // ── Real-time Stats Accumulation ───────────────────────────────────
    // Called from the hook callback (hot path — must be fast).
    // Uses a mutex, but contention is minimal: single writer (hook thread)
    // vs occasional reader (GUI recompute, ~10 Hz).
    void pushSample(const DeltaSample& sample);

    // ── Parameter Re-computation ──────────────────────────────────────
    // Analyzes accumulated samples and updates the active profile's
    // derived parameters (recommendedBeta, recommendedMinCutoff, etc.).
    // Call from GUI thread periodically (~5-10 Hz).
    void recomputeParameters();

    // ── Session Management ─────────────────────────────────────────────
    void startSession();     // Reset stats, record start time
    void endSession();       // Final recompute, optional auto-save

    // ── Apply profile to InputProcessor ───────────────────────────────
    // Constructs a FilterProfile from active() and pushes it downstream
    void applyToProcessor(InputProcessor& processor);

    // ── Statistics Access (for GUI) ────────────────────────────────────
    size_t   sampleCount() const;
    double   sessionDurationS() const;
    uint64_t totalSamples() const { return m_active.totalSamples; }

    // Snapshot of recent samples (for GUI plotting). Returns a copy.
    std::vector<DeltaSample> sampleSnapshot(size_t maxCount = 1000) const;

    // ── Profile directory ──────────────────────────────────────────────
    const std::string& profileDir() const { return m_profileDir; }

private:
    // ── Tremor frequency estimation (zero-crossing rate) ──────────────
    double estimateTremorFrequency() const;

    // ── Overshoot ratio estimation ────────────────────────────────────
    double estimateOvershootRatio() const;

    // ── Fatigue estimation (speed degradation over time) ──────────────
    double estimateFatigueRate() const;

    // ── Data ───────────────────────────────────────────────────────────
    std::string          m_profileDir;
    UserAbilityProfile   m_active;

    // Sample buffer: protected by mutex. Hook writes, GUI reads.
    mutable std::mutex   m_sampleMutex;
    std::vector<DeltaSample> m_samples;
    static constexpr size_t MAX_SAMPLES = 6000;  // ~6s at 1000 Hz

    // Session timing
    std::chrono::steady_clock::time_point m_sessionStart;
    bool m_sessionActive = false;
};

} // namespace aether
