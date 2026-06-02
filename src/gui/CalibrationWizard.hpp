#pragma once
// ============================================================================
// CalibrationWizard.hpp — Guided Motor Assessment
//
// Four sequential tests that measure the user's motor characteristics:
//   1. Steady Aim   — Hold still on a target → tremor frequency + severity
//   2. Flick        — Flick to random targets   → overshoot tendency
//   3. Tracking     — Track a moving target      → reaction lag + fatigue
//   4. Reaction     — Click when target changes  → raw reaction time
//
// Results auto-populate UserAbilityProfile fields and feed into
// OneEuroFilter::betaFromSeverity() for parameter recommendations.
// ============================================================================

#include "imgui.h"
#include "config/ProfileData.hpp"
#include "core/MouseHookManager.hpp"
#include <vector>
#include <cmath>
#include <cstdint>
#include <chrono>

namespace aether {

// ============================================================================
// Per-test result structures
// ============================================================================

struct SteadyAimResult {
    double tremorFreqHz    = 0.0;   // Dominant tremor frequency (zero-crossing)
    double tremorSeverityX = 0.0;   // RMS jitter on X axis (px)
    double tremorSeverityY = 0.0;   // RMS jitter on Y axis (px)
    double avgJitterPx     = 0.0;   // Combined RMS
    int    sampleCount     = 0;
};

struct FlickResult {
    double overshootRatio   = 0.0;  // Fraction of flicks that overshot
    double avgOvershootPx   = 0.0;  // Average overshoot distance
    double avgFlickSpeed    = 0.0;  // Average peak speed during flick (px/s)
    int    totalFlicks      = 0;
    int    overshootCount   = 0;
};

struct TrackingResult {
    double trackingErrorRMS = 0.0;  // RMS distance from cursor to target (px)
    double fatigueRate      = 0.0;  // Error increase per second (px/s)
    double avgLagPx         = 0.0;  // Average lag behind target (px)
    double maxErrorPx       = 0.0;  // Worst single-frame error
    int    sampleCount      = 0;
};

struct ReactionResult {
    double avgReactionMs    = 0.0;
    double minReactionMs    = 0.0;
    double maxReactionMs    = 0.0;
    double medianReactionMs = 0.0;
    int    totalTests       = 0;
    int    earlyClicks      = 0;    // Clicked before green
};

// ============================================================================
// Aggregate calibration results
// ============================================================================

struct CalibrationResults {
    SteadyAimResult  steadyAim;
    FlickResult      flick;
    TrackingResult   tracking;
    ReactionResult   reaction;

    // Derived overall scores (0-10)
    double overallTremorSeverity = 0.0;
    double overallOvershoot      = 0.0;
    double overallFatigue        = 0.0;
    double overallReactionDelay  = 0.0;

    bool isValid() const {
        return steadyAim.sampleCount > 0 || flick.totalFlicks > 0
            || tracking.sampleCount > 0 || reaction.totalTests > 0;
    }
};

// ============================================================================
// Calibration sample (one raw mouse event during a test)
// ============================================================================
struct CalibrationSample {
    double time;           // Seconds since test start
    double rawDx, rawDy;   // Raw mouse delta
    double cursorX;        // Virtual cursor X in canvas space
    double cursorY;        // Virtual cursor Y in canvas space
};

// ============================================================================
// CalibrationConfig
// ============================================================================
struct CalibrationConfig {
    float steadyAimDurationS  = 5.0f;
    int   flickRepetitions    = 8;
    float trackingDurationS   = 20.0f;
    int   reactionRepetitions = 6;
    float targetRadius        = 20.0f;   // Visual target size (canvas px)
    float flickMinDistance    = 80.0f;   // Minimum flick distance (canvas px)
    float flickMaxDistance    = 250.0f;  // Maximum flick distance
    float trackingSpeed       = 60.0f;   // Target movement speed (px/s)
};

// ============================================================================
// CalibrationWizard
// ============================================================================
class CalibrationWizard {
public:
    enum class Phase {
        Idle,
        // Steady Aim
        SteadyAim_Countdown,
        SteadyAim_Active,
        SteadyAim_Result,
        // Flick
        Flick_Countdown,
        Flick_AwaitReturn,
        Flick_Active,
        Flick_Result,
        // Tracking
        Tracking_Countdown,
        Tracking_Active,
        Tracking_Result,
        // Reaction
        Reaction_Countdown,
        Reaction_Wait,
        Reaction_Active,
        Reaction_Result,
        // Final
        Complete
    };

    CalibrationWizard();
    ~CalibrationWizard();

    // ── Lifecycle ──────────────────────────────────────────────────────
    void start();
    void stop();
    void reset();

    Phase phase() const { return m_phase; }
    bool  isComplete() const { return m_phase == Phase::Complete; }
    bool  isRunning() const { return m_phase != Phase::Idle && m_phase != Phase::Complete; }

    // ── Feed raw mouse data from hook callback (same thread — no lock) ─
    void pushMouseDelta(double dx, double dy, double dt);

    // ── Render current test into the given canvas region ───────────────
    void render(const ImVec2& canvasPos, const ImVec2& canvasSize);

    // ── Results ────────────────────────────────────────────────────────
    const CalibrationResults& results() const { return m_results; }

    // Apply calibration findings to a user profile
    void applyToProfile(UserAbilityProfile& profile);

    // ── Progress (0.0 to 1.0) ──────────────────────────────────────────
    float progress() const;

    // ── Get phase name for UI display ──────────────────────────────────
    const char* phaseName() const;

private:
    // ── Test renderers ─────────────────────────────────────────────────
    void renderSteadyAim();
    void renderFlickTest();
    void renderTrackingTest();
    void renderReactionTest();
    void renderResults();

    // ── Helpers ────────────────────────────────────────────────────────
    void renderCountdownOverlay(const char* testName, float remaining);
    void renderCrosshair(float cx, float cy, float size, ImU32 color);
    void drawTarget(float cx, float cy, float radius, ImU32 fill, ImU32 outline);

    void transitionTo(Phase newPhase);

    // ── Steady-aim analysis ───────────────────────────────────────────
    void analyzeSteadyAim();

    // ── Flick analysis ────────────────────────────────────────────────
    void analyzeFlickTest();

    // ── Tracking analysis ─────────────────────────────────────────────
    void analyzeTrackingTest();

    // ── Reaction analysis ─────────────────────────────────────────────
    void analyzeReaction();

    // ── State ──────────────────────────────────────────────────────────
    Phase              m_phase   = Phase::Idle;
    CalibrationConfig  m_config;
    CalibrationResults m_results;

    // Timing
    std::chrono::steady_clock::time_point m_phaseStart;
    std::chrono::steady_clock::time_point m_testStart;
    float  m_phaseElapsed = 0.0f;
    float  m_testElapsed  = 0.0f;
    float  m_countdownRemaining = 3.0f;

    // Canvas geometry
    float m_canvasW = 600.0f;
    float m_canvasH = 400.0f;
    float m_cursorX = 300.0f;   // Virtual cursor position in canvas
    float m_cursorY = 200.0f;

    // ── Steady Aim state ───────────────────────────────────────────────
    float m_steadyTargetX = 0.0f;
    float m_steadyTargetY = 0.0f;
    std::vector<CalibrationSample> m_steadySamples;

    // ── Flick test state ───────────────────────────────────────────────
    int   m_flickIndex       = 0;
    int   m_flickOvershootCount = 0;
    float m_flickTargetX     = 0.0f;
    float m_flickTargetY     = 0.0f;
    float m_flickStartX      = 0.0f;
    float m_flickStartY      = 0.0f;
    bool  m_flickReturnedToStart = false;
    bool  m_flickOvershot    = false;
    float m_flickPeakSpeed   = 0.0f;
    float m_flickMaxDist     = 0.0f;   // Max distance reached in this flick
    float m_flickTargetDist  = 0.0f;   // Actual target distance
    std::vector<float> m_flickOvershootDistances;
    std::vector<float> m_flickPeakSpeeds;
    std::vector<CalibrationSample> m_flickSamples;

    // ── Tracking state ─────────────────────────────────────────────────
    float m_trackTargetX     = 0.0f;
    float m_trackTargetY     = 0.0f;
    float m_trackAngle       = 0.0f;
    float m_trackErrorAccum  = 0.0f;
    std::vector<CalibrationSample> m_trackErrors;  // time + error pairs

    // ── Reaction state ─────────────────────────────────────────────────
    int   m_reactionIndex    = 0;
    float m_reactionWaitTime = 0.0f;   // Random delay before color change
    bool  m_reactionTargetGreen = false;
    std::vector<float> m_reactionTimes;
    int   m_reactionEarlyClicks = 0;

    // ── Drawing helpers ────────────────────────────────────────────────
    ImVec2 m_canvasScreenPos;   // Set each frame before render
    ImVec2 m_canvasScreenSize;
};

} // namespace aether
