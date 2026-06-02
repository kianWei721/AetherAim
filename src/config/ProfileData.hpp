#pragma once
// ============================================================================
// ProfileData.hpp — User Ability Profile & Statistics Types
//
// Two main data types:
//   UserAbilityProfile  — Persistent: saved as JSON per user.
//                          Captures motor characteristics, preferences,
//                          and auto-tuned filter parameters.
//   DeltaSample         — Ephemeral: a single mouse event for stats.
//                          Accumulated in-memory during a session.
//
// The profile drives the filter: tremor severity → β, overshoot → cutoff, etc.
// ============================================================================

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace aether {

// ============================================================================
// UserAbilityProfile — per-user motor assessment + preferences
//
// Populated via:
//   1. Initial calibration (guided aim test → tremor/overshoot/reaction scores)
//   2. Ongoing adaptation (session stats → refined frequency/overshoot estimates)
//   3. Manual override (user knows their condition best)
// ============================================================================
struct UserAbilityProfile {
    // ── Identity ───────────────────────────────────────────────────────
    std::string name;            // Display name
    std::string game;            // Game this profile is tuned for ("cs2", "valorant", etc.)
    std::string created;         // ISO 8601 creation date
    std::string lastUsed;        // ISO 8601 last session date
    std::string notes;           // Free-text notes (condition, medication, etc.)

    // ── Motor Assessment (0.0 = no impairment, 10.0 = severe) ─────────
    double tremorSeverity    = 0.0;   // High-freq shake: Parkinson's, essential tremor
    double overshootTendency = 0.0;   // Tendency to flick past target (cerebellar)
    double fatigueRate       = 0.0;   // Accuracy degradation over session duration
    double reactionDelayMs   = 0.0;   // Additional delay beyond baseline (~200ms)

    // ── Hand-Specific Settings ─────────────────────────────────────────
    // Separate tremor characteristics per axis (some users have anisotropic tremor)
    double tremorSeverityX   = 0.0;   // Horizontal tremor severity
    double tremorSeverityY   = 0.0;   // Vertical tremor severity (often worse for wrist aimers)

    // ── Sensitivity Preferences ────────────────────────────────────────
    double preferredSensitivity = 1.0;   // In-game sensitivity multiplier (eDPI / baseline)
    int32_t preferredDPI        = 800;   // Mouse DPI setting
    double preferredHz          = 1000.0;// Mouse polling rate

    // ── Auto-Computed Filter Parameters ────────────────────────────────
    // These are derived from the assessment scores above and continuously
    // refined by ProfileManager::recomputeParameters().
    double recommendedBeta        = 0.0;
    double recommendedMinCutoff   = 1.2;
    double recommendedSpeedCoeff  = 1.0;
    double recommendedDCutoff     = 1.0;

    // ── Session Statistics (accumulated, not persisted to JSON) ────────
    uint64_t totalSamples      = 0;
    double   avgRawSpeed       = 0.0;    // px/s — average raw mouse speed
    double   avgFilteredSpeed  = 0.0;    // px/s — average filtered speed
    double   tremorFreqHz      = 0.0;    // Dominant tremor frequency (from zero-crossings)
    double   overshootRatio    = 0.0;    // Ratio of overshooting movements
    double   avgLatencyUs      = 0.0;    // Average hook processing latency (μs)

    // ── Calibration History ────────────────────────────────────────────
    int32_t  calibrationCount = 0;
    std::string lastCalibration;  // ISO 8601

    // nlohmann::json automatic serialization
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(UserAbilityProfile,
        name, game, created, lastUsed, notes,
        tremorSeverity, overshootTendency, fatigueRate, reactionDelayMs,
        tremorSeverityX, tremorSeverityY,
        preferredSensitivity, preferredDPI, preferredHz,
        recommendedBeta, recommendedMinCutoff, recommendedSpeedCoeff, recommendedDCutoff,
        totalSamples, avgRawSpeed, avgFilteredSpeed, tremorFreqHz, overshootRatio,
        avgLatencyUs, calibrationCount, lastCalibration
    )
};

// ============================================================================
// DeltaSample — one raw mouse event (for stats accumulation)
//
// Captures before/after filtering for real-time analysis.
// Stored in a ring buffer (last N seconds of data).
// ============================================================================
struct alignas(32) DeltaSample {
    double timestamp;       // Seconds since session start (QPC-based)
    double rawDx, rawDy;    // Raw mouse delta from hook (pixels)
    double filtDx, filtDy;  // Filtered delta that was injected (pixels)
    double rawSpeed;        // 2D speed of raw input (px/s)
    double filtSpeed;       // 2D speed of filtered output (px/s)
    double dt;              // Time delta since previous sample (seconds)
};

} // namespace aether
