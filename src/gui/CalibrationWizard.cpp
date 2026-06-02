// ============================================================================
// CalibrationWizard.cpp — Guided Motor Assessment Implementation
// ============================================================================

#include "CalibrationWizard.hpp"
#include "core/OneEuroFilter.hpp"
#include "utils/MathUtils.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <numbers>
#include <cstdio>

namespace aether {

// ============================================================================
// Helpers
// ============================================================================

static float randFloat(float lo, float hi) {
    static std::mt19937 rng(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng);
}

static double elapsedSince(std::chrono::steady_clock::time_point t0) {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - t0).count();
}

// ============================================================================
// Construction
// ============================================================================
CalibrationWizard::CalibrationWizard() {
    m_steadySamples.reserve(6000);
    m_flickSamples.reserve(2000);
    m_trackErrors.reserve(20000);
    m_reactionTimes.reserve(20);
    m_flickOvershootDistances.reserve(20);
    m_flickPeakSpeeds.reserve(20);
}

CalibrationWizard::~CalibrationWizard() = default;

// ============================================================================
// Lifecycle
// ============================================================================
void CalibrationWizard::start() {
    reset();
    transitionTo(Phase::SteadyAim_Countdown);
}

void CalibrationWizard::stop() {
    m_phase = Phase::Idle;
}

void CalibrationWizard::reset() {
    m_phase   = Phase::Idle;
    m_results = CalibrationResults{};
    m_steadySamples.clear();
    m_flickSamples.clear();
    m_flickOvershootDistances.clear();
    m_flickPeakSpeeds.clear();
    m_trackErrors.clear();
    m_reactionTimes.clear();
    m_flickIndex          = 0;
    m_flickOvershootCount = 0;
    m_reactionIndex       = 0;
    m_reactionEarlyClicks = 0;
    m_cursorX = m_canvasW * 0.5f;
    m_cursorY = m_canvasH * 0.5f;
}

// ============================================================================
// pushMouseDelta — receive raw mouse data from hook callback
// ============================================================================
void CalibrationWizard::pushMouseDelta(double dx, double dy, double dt) {
    if (m_phase == Phase::Idle || m_phase == Phase::Complete) return;

    // Update virtual cursor position (clamped to canvas)
    m_cursorX = std::clamp(m_cursorX + static_cast<float>(dx), 0.0f, m_canvasW);
    m_cursorY = std::clamp(m_cursorY + static_cast<float>(dy), 0.0f, m_canvasH);

    double testTime = elapsedSince(m_testStart);

    CalibrationSample sample;
    sample.time    = testTime;
    sample.rawDx   = dx;
    sample.rawDy   = dy;
    sample.cursorX = m_cursorX;
    sample.cursorY = m_cursorY;

    switch (m_phase) {
    case Phase::SteadyAim_Active:
        m_steadySamples.push_back(sample);
        break;

    case Phase::Flick_Active: {
        m_flickSamples.push_back(sample);
        // Track peak distance from start
        float distFromStart = std::hypot(
            m_cursorX - m_flickStartX,
            m_cursorY - m_flickStartY);
        if (distFromStart > m_flickMaxDist) {
            m_flickMaxDist = distFromStart;
        }
        // Track peak instantaneous speed
        float speed = static_cast<float>(
            std::sqrt(dx * dx + dy * dy) / std::max(dt, 0.001));
        if (speed > m_flickPeakSpeed) {
            m_flickPeakSpeed = speed;
        }
        // Overshoot detection
        if (!m_flickOvershot && distFromStart > m_flickTargetDist + m_config.targetRadius) {
            m_flickOvershot = true;
        }
        break;
    }

    case Phase::Tracking_Active: {
        float dist = std::hypot(
            m_cursorX - m_trackTargetX,
            m_cursorY - m_trackTargetY);
        m_trackErrorAccum += dist;
        CalibrationSample errSample = sample;
        errSample.rawDx = dist;     // Repurpose rawDx to store error
        m_trackErrors.push_back(errSample);
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// transitionTo — state machine transition
// ============================================================================
void CalibrationWizard::transitionTo(Phase newPhase) {
    m_phase        = newPhase;
    m_phaseStart   = std::chrono::steady_clock::now();
    m_phaseElapsed = 0.0f;

    switch (newPhase) {
    case Phase::SteadyAim_Countdown:
        m_countdownRemaining = 3.0f;
        m_steadySamples.clear();
        m_steadyTargetX = m_canvasW * 0.5f;
        m_steadyTargetY = m_canvasH * 0.5f;
        // Snap cursor to target area for fair start
        m_cursorX = m_steadyTargetX;
        m_cursorY = m_steadyTargetY;
        break;

    case Phase::SteadyAim_Active:
        m_testStart  = std::chrono::steady_clock::now();
        m_testElapsed = 0.0f;
        break;

    case Phase::SteadyAim_Result:
        analyzeSteadyAim();
        break;

    case Phase::Flick_Countdown:
        m_countdownRemaining = 3.0f;
        m_flickIndex = 0;
        m_flickOvershootCount = 0;
        m_flickOvershootDistances.clear();
        m_flickPeakSpeeds.clear();
        m_flickStartX = m_canvasW * 0.5f;
        m_flickStartY = m_canvasH * 0.5f;
        m_cursorX = m_flickStartX;
        m_cursorY = m_flickStartY;
        break;

    case Phase::Flick_AwaitReturn:
        // User must return cursor to start position
        break;

    case Phase::Flick_Active:
        m_flickSamples.clear();
        m_flickOvershot   = false;
        m_flickMaxDist    = 0.0f;
        m_flickPeakSpeed  = 0.0f;
        // Place target at random position
        {
            float angle = randFloat(0.0f, 2.0f * std::numbers::pi_v<float>);
            float dist  = randFloat(m_config.flickMinDistance, m_config.flickMaxDistance);
            m_flickTargetX = std::clamp(
                m_flickStartX + dist * std::cos(angle),
                m_config.targetRadius, m_canvasW - m_config.targetRadius);
            m_flickTargetY = std::clamp(
                m_flickStartY + dist * std::sin(angle),
                m_config.targetRadius, m_canvasH - m_config.targetRadius);
            m_flickTargetDist = std::hypot(
                m_flickTargetX - m_flickStartX,
                m_flickTargetY - m_flickStartY);
        }
        break;

    case Phase::Flick_Result:
        analyzeFlickTest();
        break;

    case Phase::Tracking_Countdown:
        m_countdownRemaining = 3.0f;
        m_trackErrors.clear();
        m_trackErrorAccum = 0.0f;
        m_trackAngle      = 0.0f;
        m_trackTargetX    = m_canvasW * 0.5f;
        m_trackTargetY    = m_canvasH * 0.5f;
        m_cursorX = m_trackTargetX;
        m_cursorY = m_trackTargetY;
        break;

    case Phase::Tracking_Active:
        m_testStart  = std::chrono::steady_clock::now();
        m_testElapsed = 0.0f;
        break;

    case Phase::Tracking_Result:
        analyzeTrackingTest();
        break;

    case Phase::Reaction_Countdown:
        m_countdownRemaining = 2.0f;
        m_reactionIndex = 0;
        m_reactionEarlyClicks = 0;
        m_reactionTimes.clear();
        break;

    case Phase::Reaction_Wait:
        m_reactionWaitTime     = randFloat(1.0f, 3.0f);
        m_reactionTargetGreen  = false;
        break;

    case Phase::Reaction_Active:
        m_testStart = std::chrono::steady_clock::now();
        break;

    case Phase::Reaction_Result:
        analyzeReaction();
        break;

    case Phase::Complete:
        // Derive overall scores
        {
            auto& r = m_results;
            r.overallTremorSeverity = std::clamp(
                (r.steadyAim.tremorSeverityX + r.steadyAim.tremorSeverityY) * 0.5f * 2.0, 0.0, 10.0);
            r.overallOvershoot = std::clamp(r.flick.overshootRatio * 12.0, 0.0, 10.0);
            r.overallFatigue   = std::clamp(r.tracking.fatigueRate * 50.0, 0.0, 10.0);
            r.overallReactionDelay = std::clamp(r.reaction.avgReactionMs - 180.0, 0.0, 200.0) / 20.0;
        }
        break;

    default:
        break;
    }
}

// ============================================================================
// progress — 0.0 to 1.0 overall calibration progress
// ============================================================================
float CalibrationWizard::progress() const {
    constexpr float weights[] = { 0.25f, 0.25f, 0.30f, 0.20f };
    switch (m_phase) {
    case Phase::Idle:              return 0.0f;
    case Phase::SteadyAim_Countdown:
    case Phase::SteadyAim_Active:  return 0.05f;
    case Phase::SteadyAim_Result:  return weights[0];
    case Phase::Flick_Countdown:
    case Phase::Flick_AwaitReturn:
    case Phase::Flick_Active:      return weights[0] + 0.05f;
    case Phase::Flick_Result:      return weights[0] + weights[1];
    case Phase::Tracking_Countdown:
    case Phase::Tracking_Active:   return weights[0] + weights[1] + 0.05f;
    case Phase::Tracking_Result:   return weights[0] + weights[1] + weights[2];
    case Phase::Reaction_Countdown:
    case Phase::Reaction_Wait:
    case Phase::Reaction_Active:   return weights[0] + weights[1] + weights[2] + 0.05f;
    case Phase::Reaction_Result:
    case Phase::Complete:          return 1.0f;
    }
    return 0.0f;
}

const char* CalibrationWizard::phaseName() const {
    switch (m_phase) {
    case Phase::Idle:                return "Ready";
    case Phase::SteadyAim_Countdown:
    case Phase::SteadyAim_Active:    return "Steady Aim";
    case Phase::SteadyAim_Result:    return "Steady Aim — Results";
    case Phase::Flick_Countdown:
    case Phase::Flick_AwaitReturn:
    case Phase::Flick_Active:        return "Flick Test";
    case Phase::Flick_Result:        return "Flick Test — Results";
    case Phase::Tracking_Countdown:
    case Phase::Tracking_Active:     return "Tracking";
    case Phase::Tracking_Result:     return "Tracking — Results";
    case Phase::Reaction_Countdown:
    case Phase::Reaction_Wait:
    case Phase::Reaction_Active:     return "Reaction";
    case Phase::Reaction_Result:     return "Reaction — Results";
    case Phase::Complete:            return "Calibration Complete";
    }
    return "";
}

// ============================================================================
// applyToProfile
// ============================================================================
void CalibrationWizard::applyToProfile(UserAbilityProfile& profile) {
    auto& r = m_results;

    profile.tremorSeverity    = r.overallTremorSeverity;
    profile.tremorSeverityX   = r.steadyAim.tremorSeverityX * 2.0;
    profile.tremorSeverityY   = r.steadyAim.tremorSeverityY * 2.0;
    profile.overshootTendency = r.overallOvershoot;
    profile.fatigueRate       = r.overallFatigue;
    profile.reactionDelayMs   = r.overallReactionDelay;

    // Auto-compute recommended filter parameters
    int tremorLevel = static_cast<int>(std::round(r.overallTremorSeverity));
    profile.recommendedBeta       = OneEuroFilter::betaFromSeverity(tremorLevel);
    profile.recommendedMinCutoff  = std::clamp(0.5 + r.steadyAim.tremorFreqHz * 0.15, 0.1, 8.0);
    profile.recommendedSpeedCoeff = 1.0 + r.overallOvershoot * 0.5;
    profile.recommendedDCutoff    = 1.0;

    profile.calibrationCount++;
    profile.lastCalibration = []() -> std::string {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
        localtime_s(&tm, &now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        return buf;
    }();
}

// ============================================================================
// RENDER DISPATCH
// ============================================================================
void CalibrationWizard::render(const ImVec2& canvasPos, const ImVec2& canvasSize) {
    m_canvasScreenPos  = canvasPos;
    m_canvasScreenSize = canvasSize;
    m_canvasW = canvasSize.x;
    m_canvasH = canvasSize.y;

    switch (m_phase) {
    case Phase::SteadyAim_Countdown:
    case Phase::SteadyAim_Active:
        renderSteadyAim();
        break;
    case Phase::Flick_Countdown:
    case Phase::Flick_AwaitReturn:
    case Phase::Flick_Active:
        renderFlickTest();
        break;
    case Phase::Tracking_Countdown:
    case Phase::Tracking_Active:
        renderTrackingTest();
        break;
    case Phase::Reaction_Countdown:
    case Phase::Reaction_Wait:
    case Phase::Reaction_Active:
        renderReactionTest();
        break;
    case Phase::Complete:
    case Phase::SteadyAim_Result:
    case Phase::Flick_Result:
    case Phase::Tracking_Result:
    case Phase::Reaction_Result:
        renderResults();
        break;
    default:
        break;
    }

    // Timers
    auto now = std::chrono::steady_clock::now();
    m_phaseElapsed = std::chrono::duration<float>(now - m_phaseStart).count();
    m_testElapsed  = std::chrono::duration<float>(now - m_testStart).count();
}

// ============================================================================
// DRAWING HELPERS
// ============================================================================
void CalibrationWizard::renderCrosshair(float cx, float cy, float size, ImU32 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float sx = m_canvasScreenPos.x + cx;
    float sy = m_canvasScreenPos.y + cy;
    dl->AddLine(ImVec2(sx - size, sy), ImVec2(sx + size, sy), color, 1.5f);
    dl->AddLine(ImVec2(sx, sy - size), ImVec2(sx, sy + size), color, 1.5f);
    dl->AddCircle(ImVec2(sx, sy), size * 0.4f, color, 12, 1.0f);
}

void CalibrationWizard::drawTarget(float cx, float cy, float radius,
                                    ImU32 fill, ImU32 outline) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float sx = m_canvasScreenPos.x + cx;
    float sy = m_canvasScreenPos.y + cy;
    dl->AddCircleFilled(ImVec2(sx, sy), radius, fill);
    dl->AddCircle(ImVec2(sx, sy), radius, outline, 0, 2.0f);
    // Inner dot
    dl->AddCircleFilled(ImVec2(sx, sy), 3.0f, IM_COL32(255, 255, 255, 200));
}

void CalibrationWizard::renderCountdownOverlay(const char* testName, float remaining) {
    int count = static_cast<int>(std::ceil(remaining));
    float cx = m_canvasScreenPos.x + m_canvasW * 0.5f;
    float cy = m_canvasScreenPos.y + m_canvasH * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Semi-transparent overlay
    dl->AddRectFilled(m_canvasScreenPos,
        ImVec2(m_canvasScreenPos.x + m_canvasW, m_canvasScreenPos.y + m_canvasH),
        IM_COL32(0, 0, 0, 120));

    // Test name
    dl->AddText(ImVec2(cx - 80, cy - 60), IM_COL32(200, 200, 220, 255), testName);

    // Countdown number
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", count);
    ImVec2 numSize = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(cx - numSize.x * 0.5f, cy - numSize.y * 0.5f),
        IM_COL32(255, 255, 255, 255), buf);
}

// ============================================================================
// STEADY AIM TEST
// ============================================================================
void CalibrationWizard::renderSteadyAim() {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(m_canvasScreenPos,
        ImVec2(m_canvasScreenPos.x + m_canvasW, m_canvasScreenPos.y + m_canvasH),
        IM_COL32(18, 20, 28, 255));

    // Target (crosshair)
    float tx = m_steadyTargetX;
    float ty = m_steadyTargetY;
    drawTarget(tx, ty, m_config.targetRadius,
               IM_COL32(255, 80, 80, 80), IM_COL32(255, 100, 100, 255));

    // Cursor indicator
    float cx = m_cursorX, cy = m_cursorY;
    renderCrosshair(cx, cy, 12.0f, IM_COL32(0, 220, 255, 200));

    // Distance line from cursor to target
    float sx = m_canvasScreenPos.x + cx, sy = m_canvasScreenPos.y + cy;
    float stx = m_canvasScreenPos.x + tx, sty = m_canvasScreenPos.y + ty;
    dl->AddLine(ImVec2(sx, sy), ImVec2(stx, sty),
                IM_COL32(255, 255, 255, 30), 1.0f);

    // Jitter trail (last ~50 samples)
    if (m_steadySamples.size() > 1) {
        size_t start = m_steadySamples.size() > 50
            ? m_steadySamples.size() - 50 : 0;
        for (size_t i = start + 1; i < m_steadySamples.size(); ++i) {
            float x1 = m_canvasScreenPos.x + static_cast<float>(m_steadySamples[i-1].cursorX);
            float y1 = m_canvasScreenPos.y + static_cast<float>(m_steadySamples[i-1].cursorY);
            float x2 = m_canvasScreenPos.x + static_cast<float>(m_steadySamples[i].cursorX);
            float y2 = m_canvasScreenPos.y + static_cast<float>(m_steadySamples[i].cursorY);
            dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2),
                        IM_COL32(0, 200, 255, 80), 1.0f);
        }
    }

    // Instructions
    float ix = m_canvasScreenPos.x + 10, iy = m_canvasScreenPos.y + 10;
    dl->AddText(ImVec2(ix, iy), IM_COL32(200, 200, 220, 255),
        "Hold the cursor STILL on the red target");

    // Phase-specific
    if (m_phase == Phase::SteadyAim_Countdown) {
        renderCountdownOverlay("Steady Aim", m_countdownRemaining - m_phaseElapsed);

        if (m_phaseElapsed >= m_countdownRemaining) {
            transitionTo(Phase::SteadyAim_Active);
        }
    } else if (m_phase == Phase::SteadyAim_Active) {
        float remain = m_config.steadyAimDurationS - m_testElapsed;
        char buf[32];
        snprintf(buf, sizeof(buf), "Hold still... %.1f s", remain);
        dl->AddText(ImVec2(ix, iy + 20), IM_COL32(255, 220, 100, 255), buf);

        // Progress bar
        float barW = m_canvasW - 20;
        float progress = std::clamp(m_testElapsed / m_config.steadyAimDurationS, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(ix, iy + 40), ImVec2(ix + barW, iy + 48),
                          IM_COL32(40, 40, 50, 255));
        dl->AddRectFilled(ImVec2(ix, iy + 40), ImVec2(ix + barW * progress, iy + 48),
                          IM_COL32(0, 180, 220, 255));

        if (m_testElapsed >= m_config.steadyAimDurationS) {
            transitionTo(Phase::SteadyAim_Result);
        }
    }
}

// ============================================================================
// FLICK TEST
// ============================================================================
void CalibrationWizard::renderFlickTest() {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(m_canvasScreenPos,
        ImVec2(m_canvasScreenPos.x + m_canvasW, m_canvasScreenPos.y + m_canvasH),
        IM_COL32(18, 20, 28, 255));

    float ix = m_canvasScreenPos.x + 10, iy = m_canvasScreenPos.y + 10;

    // Phase-specific UI
    if (m_phase == Phase::Flick_Countdown) {
        drawTarget(m_flickStartX, m_flickStartY, m_config.targetRadius,
                   IM_COL32(80, 80, 255, 80), IM_COL32(100, 100, 255, 200));
        renderCrosshair(m_cursorX, m_cursorY, 12.0f, IM_COL32(0, 220, 255, 200));
        renderCountdownOverlay("Flick Test", m_countdownRemaining - m_phaseElapsed);

        dl->AddText(ImVec2(ix, iy), IM_COL32(200, 200, 220, 255),
            "Flick to targets as quickly as possible");

        if (m_phaseElapsed >= m_countdownRemaining) {
            transitionTo(Phase::Flick_Active);
        }
    }
    else if (m_phase == Phase::Flick_AwaitReturn) {
        // Start position marker
        drawTarget(m_flickStartX, m_flickStartY, m_config.targetRadius * 0.7f,
                   IM_COL32(80, 80, 255, 60), IM_COL32(100, 100, 255, 150));
        renderCrosshair(m_cursorX, m_cursorY, 12.0f, IM_COL32(0, 220, 255, 200));

        float distToStart = std::hypot(m_cursorX - m_flickStartX,
                                        m_cursorY - m_flickStartY);
        dl->AddText(ImVec2(ix, iy), IM_COL32(200, 220, 100, 255),
            "Return to the blue start position");
        char dbuf[32];
        snprintf(dbuf, sizeof(dbuf), "Distance: %.0f px", distToStart);
        dl->AddText(ImVec2(ix, iy + 20), IM_COL32(180, 180, 200, 255), dbuf);

        if (distToStart < m_config.targetRadius) {
            m_flickReturnedToStart = true;
            transitionTo(Phase::Flick_Active);
        }

        // Check for click (= user wants to skip to next)
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
            m_cursorX = m_flickStartX;
            m_cursorY = m_flickStartY;
            m_flickReturnedToStart = true;
            transitionTo(Phase::Flick_Active);
        }
    }
    else if (m_phase == Phase::Flick_Active) {
        // Draw start position faintly
        drawTarget(m_flickStartX, m_flickStartY, 4.0f,
                   IM_COL32(80, 80, 255, 100), IM_COL32(100, 100, 255, 100));

        // Draw target
        drawTarget(m_flickTargetX, m_flickTargetY, m_config.targetRadius,
                   IM_COL32(255, 200, 50, 80), IM_COL32(255, 220, 80, 255));

        // Draw flick trail
        if (m_flickSamples.size() > 1) {
            for (size_t i = 1; i < m_flickSamples.size(); ++i) {
                float x1 = m_canvasScreenPos.x + static_cast<float>(m_flickSamples[i-1].cursorX);
                float y1 = m_canvasScreenPos.y + static_cast<float>(m_flickSamples[i-1].cursorY);
                float x2 = m_canvasScreenPos.x + static_cast<float>(m_flickSamples[i].cursorX);
                float y2 = m_canvasScreenPos.y + static_cast<float>(m_flickSamples[i].cursorY);
                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2),
                            IM_COL32(255, 180, 50, 150), 2.0f);
            }
        }

        // Cursor
        renderCrosshair(m_cursorX, m_cursorY, 12.0f, IM_COL32(0, 220, 255, 200));

        char buf[64];
        snprintf(buf, sizeof(buf), "Flick %d/%d — Click when on target",
                 m_flickIndex + 1, m_config.flickRepetitions);
        dl->AddText(ImVec2(ix, iy), IM_COL32(255, 220, 100, 255), buf);

        // Distance to target
        float distToTarget = std::hypot(m_cursorX - m_flickTargetX,
                                         m_cursorY - m_flickTargetY);
        char dbuf[32];
        snprintf(dbuf, sizeof(dbuf), "Dist: %.0f px", distToTarget);
        dl->AddText(ImVec2(ix, iy + 20), IM_COL32(180, 180, 200, 255), dbuf);

        // Click to confirm arrival → next flick
        static bool prevClick = false;
        bool curClick = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (curClick && !prevClick) {
            // Record overshoot
            if (m_flickOvershot) {
                m_flickOvershootCount++;
                float overshootPx = m_flickMaxDist - m_flickTargetDist;
                m_flickOvershootDistances.push_back(overshootPx);
            }
            m_flickPeakSpeeds.push_back(m_flickPeakSpeed);

            m_flickIndex++;
            m_flickReturnedToStart = false;

            if (m_flickIndex >= m_config.flickRepetitions) {
                transitionTo(Phase::Flick_Result);
            } else {
                // Reset to start position for next flick
                m_flickStartX = m_cursorX;  // New start = current position
                m_flickStartY = m_cursorY;
                transitionTo(Phase::Flick_Active);
            }
        }
        prevClick = curClick;
    }
}

// ============================================================================
// TRACKING TEST
// ============================================================================
void CalibrationWizard::renderTrackingTest() {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(m_canvasScreenPos,
        ImVec2(m_canvasScreenPos.x + m_canvasW, m_canvasScreenPos.y + m_canvasH),
        IM_COL32(18, 20, 28, 255));

    float ix = m_canvasScreenPos.x + 10, iy = m_canvasScreenPos.y + 10;

    if (m_phase == Phase::Tracking_Countdown) {
        drawTarget(m_canvasW * 0.5f, m_canvasH * 0.5f, m_config.targetRadius,
                   IM_COL32(80, 255, 80, 80), IM_COL32(100, 255, 100, 200));
        renderCrosshair(m_cursorX, m_cursorY, 12.0f, IM_COL32(0, 220, 255, 200));
        renderCountdownOverlay("Tracking", m_countdownRemaining - m_phaseElapsed);

        if (m_phaseElapsed >= m_countdownRemaining) {
            transitionTo(Phase::Tracking_Active);
        }
    }
    else if (m_phase == Phase::Tracking_Active) {
        // Update target position (Lissajous curve)
        float t = m_testElapsed;
        float ax = m_canvasW * 0.35f;
        float ay = m_canvasH * 0.35f;
        m_trackTargetX = m_canvasW * 0.5f + ax * std::sin(0.7f * t);
        m_trackTargetY = m_canvasH * 0.5f + ay * std::sin(1.1f * t + 1.2f);

        // Target trail (last 60 positions)
        constexpr int trailLen = 60;
        static float trailX[trailLen], trailY[trailLen];
        static int trailIdx = 0;
        trailX[trailIdx] = m_trackTargetX;
        trailY[trailIdx] = m_trackTargetY;
        trailIdx = (trailIdx + 1) % trailLen;

        // Draw trail
        for (int i = 1; i < trailLen; ++i) {
            int idx0 = (trailIdx - i - 1 + trailLen) % trailLen;
            int idx1 = (trailIdx - i + trailLen) % trailLen;
            float a = 1.0f - static_cast<float>(i) / trailLen;
            dl->AddLine(
                ImVec2(m_canvasScreenPos.x + trailX[idx0], m_canvasScreenPos.y + trailY[idx0]),
                ImVec2(m_canvasScreenPos.x + trailX[idx1], m_canvasScreenPos.y + trailY[idx1]),
                IM_COL32(100, 255, 100, static_cast<int>(a * 60)), 2.0f);
        }

        // Target
        drawTarget(m_trackTargetX, m_trackTargetY, m_config.targetRadius * 0.8f,
                   IM_COL32(80, 255, 80, 100), IM_COL32(100, 255, 100, 255));

        // Cursor
        renderCrosshair(m_cursorX, m_cursorY, 12.0f, IM_COL32(0, 220, 255, 200));

        // Error indicator (line from cursor to target)
        float errDist = std::hypot(m_cursorX - m_trackTargetX,
                                    m_cursorY - m_trackTargetY);
        dl->AddLine(
            ImVec2(m_canvasScreenPos.x + m_cursorX, m_canvasScreenPos.y + m_cursorY),
            ImVec2(m_canvasScreenPos.x + m_trackTargetX, m_canvasScreenPos.y + m_trackTargetY),
            errDist > m_config.targetRadius
                ? IM_COL32(255, 80, 80, 60) : IM_COL32(80, 255, 80, 60), 1.0f);

        // Stats
        char buf[64];
        float remain = m_config.trackingDurationS - m_testElapsed;
        snprintf(buf, sizeof(buf), "Track the green target — %.0f s remaining", remain);
        dl->AddText(ImVec2(ix, iy), IM_COL32(200, 200, 220, 255), buf);

        snprintf(buf, sizeof(buf), "Error: %.0f px", errDist);
        dl->AddText(ImVec2(ix, iy + 20), IM_COL32(180, 200, 180, 255), buf);

        if (m_testElapsed >= m_config.trackingDurationS) {
            transitionTo(Phase::Tracking_Result);
        }
    }
}

// ============================================================================
// REACTION TEST
// ============================================================================
void CalibrationWizard::renderReactionTest() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cx = m_canvasScreenPos.x + m_canvasW * 0.5f;
    float cy = m_canvasScreenPos.y + m_canvasH * 0.5f;

    dl->AddRectFilled(m_canvasScreenPos,
        ImVec2(m_canvasScreenPos.x + m_canvasW, m_canvasScreenPos.y + m_canvasH),
        IM_COL32(18, 20, 28, 255));

    float ix = m_canvasScreenPos.x + 10, iy = m_canvasScreenPos.y + 10;

    if (m_phase == Phase::Reaction_Countdown) {
        dl->AddCircleFilled(ImVec2(cx, cy), m_config.targetRadius,
                            IM_COL32(150, 150, 150, 150));
        renderCountdownOverlay("Reaction", m_countdownRemaining - m_phaseElapsed);

        if (m_phaseElapsed >= m_countdownRemaining) {
            transitionTo(Phase::Reaction_Wait);
        }
    }
    else if (m_phase == Phase::Reaction_Wait) {
        // Red target (waiting)
        ImU32 targetColor = IM_COL32(220, 60, 60, 255);
        dl->AddCircleFilled(ImVec2(cx, cy), m_config.targetRadius, targetColor);
        dl->AddCircle(ImVec2(cx, cy), m_config.targetRadius + 3.0f,
                      IM_COL32(255, 255, 255, 40), 0, 2.0f);

        char buf[32];
        snprintf(buf, sizeof(buf), "Wait for GREEN... Test %d/%d",
                 m_reactionIndex + 1, m_config.reactionRepetitions);
        dl->AddText(ImVec2(ix, iy), IM_COL32(200, 200, 220, 255), buf);

        // Check for early clicks
        static bool prevClick2 = false;
        bool curClick2 = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (curClick2 && !prevClick2) {
            m_reactionEarlyClicks++;
            // Flash "too early"
            dl->AddText(ImVec2(cx - 40, cy + 40), IM_COL32(255, 100, 100, 255),
                        "TOO EARLY!");
        }
        prevClick2 = curClick2;

        if (m_phaseElapsed >= m_reactionWaitTime) {
            transitionTo(Phase::Reaction_Active);
        }
    }
    else if (m_phase == Phase::Reaction_Active) {
        // Green target (GO!)
        dl->AddCircleFilled(ImVec2(cx, cy), m_config.targetRadius + 5.0f,
                            IM_COL32(60, 220, 60, 255));
        dl->AddCircleFilled(ImVec2(cx, cy), m_config.targetRadius,
                            IM_COL32(80, 255, 80, 200));

        dl->AddText(ImVec2(cx - 20, cy - 40), IM_COL32(255, 255, 255, 255), "CLICK!");

        char buf[32];
        snprintf(buf, sizeof(buf), "Test %d/%d — %.0f ms",
                 m_reactionIndex + 1, m_config.reactionRepetitions,
                 m_testElapsed * 1000.0);
        dl->AddText(ImVec2(ix, iy), IM_COL32(200, 200, 220, 255), buf);

        // Detect click
        static bool prevClick3 = false;
        bool curClick3 = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (curClick3 && !prevClick3) {
            m_reactionTimes.push_back(static_cast<float>(m_testElapsed * 1000.0));
            m_reactionIndex++;

            if (m_reactionIndex >= m_config.reactionRepetitions) {
                transitionTo(Phase::Reaction_Result);
            } else {
                transitionTo(Phase::Reaction_Wait);
            }
        }
        prevClick3 = curClick3;

        // Timeout after 5s
        if (m_testElapsed > 5.0f) {
            m_reactionTimes.push_back(5000.0f);  // Timeout
            m_reactionIndex++;
            if (m_reactionIndex >= m_config.reactionRepetitions) {
                transitionTo(Phase::Reaction_Result);
            } else {
                transitionTo(Phase::Reaction_Wait);
            }
        }
    }
}

// ============================================================================
// RESULTS DISPLAY
// ============================================================================
void CalibrationWizard::renderResults() {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(m_canvasScreenPos,
        ImVec2(m_canvasScreenPos.x + m_canvasW, m_canvasScreenPos.y + m_canvasH),
        IM_COL32(22, 24, 34, 255));

    float x = m_canvasScreenPos.x + 20;
    float y = m_canvasScreenPos.y + 15;
    auto& r = m_results;

    dl->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255),
                m_phase == Phase::Complete ? "Calibration Complete!"
                                           : phaseName());
    y += 30;

    // Grid layout: 2x2
    float colW = m_canvasW * 0.5f - 30;

    auto drawCard = [&](const char* title, const char** lines, int count,
                         float cy, float cw) {
        dl->AddRectFilled(ImVec2(x, cy), ImVec2(x + cw, cy + 80),
                          IM_COL32(35, 37, 50, 255), 4.0f);
        dl->AddText(ImVec2(x + 8, cy + 4), IM_COL32(200, 220, 255, 255), title);
        for (int i = 0; i < count; ++i) {
            dl->AddText(ImVec2(x + 12, cy + 22 + i * 16),
                        IM_COL32(180, 185, 200, 255), lines[i]);
        }
    };

    // ── Steady Aim card ───────────────────────────────────────────────
    if (r.steadyAim.sampleCount > 0) {
        char buf1[64], buf2[64], buf3[64];
        snprintf(buf1, sizeof(buf1), "Tremor Freq:  %.1f Hz", r.steadyAim.tremorFreqHz);
        snprintf(buf2, sizeof(buf2), "Jitter X:     %.1f px RMS", r.steadyAim.tremorSeverityX);
        snprintf(buf3, sizeof(buf3), "Jitter Y:     %.1f px RMS", r.steadyAim.tremorSeverityY);
        const char* lines[] = { buf1, buf2, buf3 };
        drawCard("Steady Aim", lines, 3, y, colW);
    }

    // ── Flick card ────────────────────────────────────────────────────
    if (r.flick.totalFlicks > 0) {
        char buf1[64], buf2[64], buf3[64];
        snprintf(buf1, sizeof(buf1), "Overshoot:    %.0f%%", r.flick.overshootRatio * 100.0);
        snprintf(buf2, sizeof(buf2), "Avg Overshoot: %.0f px", r.flick.avgOvershootPx);
        snprintf(buf3, sizeof(buf3), "Avg Speed:    %.0f px/s", r.flick.avgFlickSpeed);
        const char* lines[] = { buf1, buf2, buf3 };
        drawCard("Flick Test", lines, 3, y, colW);
    }

    y += 100;

    // ── Tracking card ─────────────────────────────────────────────────
    if (r.tracking.sampleCount > 0) {
        char buf1[64], buf2[64], buf3[64];
        snprintf(buf1, sizeof(buf1), "RMS Error:    %.0f px", r.tracking.trackingErrorRMS);
        snprintf(buf2, sizeof(buf2), "Fatigue Rate: %.2f px/s", r.tracking.fatigueRate);
        snprintf(buf3, sizeof(buf3), "Avg Lag:      %.0f px", r.tracking.avgLagPx);
        const char* lines[] = { buf1, buf2, buf3 };
        drawCard("Tracking", lines, 3, y, colW);
    }

    // ── Reaction card ─────────────────────────────────────────────────
    if (r.reaction.totalTests > 0) {
        char buf1[64], buf2[64], buf3[64];
        snprintf(buf1, sizeof(buf1), "Avg:    %.0f ms", r.reaction.avgReactionMs);
        snprintf(buf2, sizeof(buf2), "Median: %.0f ms", r.reaction.medianReactionMs);
        snprintf(buf3, sizeof(buf3), "Early:  %d", r.reaction.earlyClicks);
        const char* lines[] = { buf1, buf2, buf3 };
        drawCard("Reaction", lines, 3, y, colW);
    }

    // Overall scores
    if (m_phase == Phase::Complete) {
        y += 110;
        char buf[128];
        snprintf(buf, sizeof(buf),
            "Overall: Tremor=%.1f  Overshoot=%.1f  Fatigue=%.1f  Reaction=%.0f ms",
            r.overallTremorSeverity, r.overallOvershoot,
            r.overallFatigue, r.overallReactionDelay);
        dl->AddText(ImVec2(x + 10, y), IM_COL32(100, 255, 180, 255), buf);
    }
}

// ============================================================================
// ANALYSIS FUNCTIONS
// ============================================================================

void CalibrationWizard::analyzeSteadyAim() {
    auto& r = m_results.steadyAim;
    r.sampleCount = static_cast<int>(m_steadySamples.size());
    if (r.sampleCount < 10) return;

    // RMS jitter per axis
    double sumSqX = 0.0, sumSqY = 0.0;
    for (auto& s : m_steadySamples) {
        sumSqX += s.rawDx * s.rawDx;
        sumSqY += s.rawDy * s.rawDy;
    }
    r.tremorSeverityX = std::sqrt(sumSqX / r.sampleCount);
    r.tremorSeverityY = std::sqrt(sumSqY / r.sampleCount);
    r.avgJitterPx     = std::sqrt((sumSqX + sumSqY) / r.sampleCount);

    // Tremor frequency via zero-crossing rate on high-pass filtered X
    int zeroCrossings = 0;
    double prevHP = 0.0;
    bool first = true;
    for (size_t i = 1; i < m_steadySamples.size(); ++i) {
        double hp = m_steadySamples[i].rawDx - m_steadySamples[i-1].rawDx;
        if (!first && prevHP * hp < 0.0) zeroCrossings++;
        prevHP = hp;
        first  = false;
    }
    if (m_steadySamples.size() >= 2) {
        double duration = m_steadySamples.back().time - m_steadySamples.front().time;
        if (duration > 0.0) r.tremorFreqHz = zeroCrossings / (2.0 * duration);
    }
}

void CalibrationWizard::analyzeFlickTest() {
    auto& r = m_results.flick;
    r.totalFlicks    = m_config.flickRepetitions;
    r.overshootCount = m_flickOvershootCount;
    r.overshootRatio = r.totalFlicks > 0
        ? static_cast<double>(r.overshootCount) / r.totalFlicks : 0.0;

    if (!m_flickOvershootDistances.empty()) {
        double sum = 0.0;
        for (float d : m_flickOvershootDistances) sum += d;
        r.avgOvershootPx = sum / m_flickOvershootDistances.size();
    }
    if (!m_flickPeakSpeeds.empty()) {
        double sum = 0.0;
        for (float s : m_flickPeakSpeeds) sum += s;
        r.avgFlickSpeed = sum / m_flickPeakSpeeds.size();
    }
}

void CalibrationWizard::analyzeTrackingTest() {
    auto& r = m_results.tracking;
    r.sampleCount = static_cast<int>(m_trackErrors.size());
    if (r.sampleCount < 10) return;

    // RMS error
    double sumSq = 0.0, sum = 0.0, maxErr = 0.0;
    for (auto& s : m_trackErrors) {
        double err = s.rawDx;  // stored as error in pushMouseDelta
        sumSq += err * err;
        sum   += err;
        if (err > maxErr) maxErr = err;
    }
    r.trackingErrorRMS = std::sqrt(sumSq / r.sampleCount);
    r.avgLagPx         = sum / r.sampleCount;
    r.maxErrorPx       = maxErr;

    // Fatigue rate: linear regression of error vs time
    // Split samples into first half and second half, compare average error
    size_t half = m_trackErrors.size() / 2;
    double firstHalfSum = 0.0, secondHalfSum = 0.0;
    for (size_t i = 0; i < m_trackErrors.size(); ++i) {
        if (i < half) firstHalfSum  += m_trackErrors[i].rawDx;
        else          secondHalfSum += m_trackErrors[i].rawDx;
    }
    double firstAvg  = firstHalfSum  / half;
    double secondAvg = secondHalfSum / (m_trackErrors.size() - half);
    double duration  = m_trackErrors.back().time - m_trackErrors.front().time;
    if (duration > 0.0) {
        r.fatigueRate = (secondAvg - firstAvg) / duration;  // px/s increase
    }
}

void CalibrationWizard::analyzeReaction() {
    auto& r = m_results.reaction;
    r.totalTests  = static_cast<int>(m_reactionTimes.size());
    r.earlyClicks = m_reactionEarlyClicks;
    if (r.totalTests == 0) return;

    // Sort for median
    std::vector<float> sorted = m_reactionTimes;
    std::sort(sorted.begin(), sorted.end());

    double sum = 0.0;
    for (float t : sorted) sum += t;
    r.avgReactionMs    = sum / r.totalTests;
    r.minReactionMs    = sorted.front();
    r.maxReactionMs    = sorted.back();
    r.medianReactionMs = sorted[sorted.size() / 2];
}

} // namespace aether
