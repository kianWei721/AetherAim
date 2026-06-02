#pragma once
// ============================================================================
// Widgets.hpp — Reusable Dear ImGui Custom Widgets
// ============================================================================

#include "imgui.h"

namespace aether::widgets {

// ── Radar / Pentagon chart for user ability dimensions ─────────────────
// values[5]: normalized [0..1] data values
// labels[5]: axis labels (static strings)
// size:      chart diameter in pixels
void DrawRadarChart(const char* title, const float values[5],
                    const char* labels[5], float size = 180.0f);

// ── Color-coded severity indicator ────────────────────────────────────
// Green (< warnAt), Orange (warnAt..dangerAt), Red (> dangerAt)
void SeverityBadge(const char* label, double value,
                   double warnAt = 4.0, double dangerAt = 7.0);

// ── (?) icon with hover tooltip ───────────────────────────────────────
void HelpMarker(const char* description);

// ── Section separator with text label ─────────────────────────────────
inline void SectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::SeparatorText(label);
}

// ── Slider with built-in HelpMarker tooltip ───────────────────────────
// Displays a float slider, and a (?) icon next to the label that shows
// parameter documentation on hover.
bool SliderWithHelp(const char* label, float* value, float min, float max,
                    const char* helpText, const char* format = "%.2f");

// ── Toggle button with active/inactive colors ─────────────────────────
bool ToggleButton(const char* label, bool* state, const ImVec2& size = ImVec2(0, 0));

// ── Mini latency display (colored by threshold) ───────────────────────
void LatencyDisplay(double latencyUs, double goodThreshold = 50.0,
                    double warnThreshold = 200.0);

} // namespace aether::widgets
