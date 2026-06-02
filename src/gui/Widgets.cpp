// ============================================================================
// Widgets.cpp — Custom ImGui Widget Implementations
// ============================================================================

#include "Widgets.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <numbers>
#include <cstdio>

namespace aether::widgets {

// ---------------------------------------------------------------------------
// DrawRadarChart — 5-axis radar/pentagon chart using ImDrawList
//
// Draws concentric reference pentagons, axis lines, labels, and a filled
// data polygon. All coordinates are in screen space relative to the
// ImGui cursor position.
// ---------------------------------------------------------------------------
void DrawRadarChart(const char* title, const float values[5],
                    const char* labels[5], float size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float cx = cursor.x + size * 0.5f;
    const float cy = cursor.y + size * 0.55f;  // slight bias for title
    const float r  = size * 0.38f;

    // Reserve space
    ImGui::Dummy(ImVec2(size, size + ImGui::GetTextLineHeight() * 2));
    ImGui::ItemSize(ImVec2(size, size));

    // Pentagon geometry: 5 vertices, top at -90° ≈ -π/2
    auto pentagonVertex = [&](int i, float radius) -> ImVec2 {
        float angle = -std::numbers::pi_v<float> * 0.5f
                      + static_cast<float>(i) * 2.0f * std::numbers::pi_v<float> / 5.0f;
        return ImVec2(cx + radius * std::cos(angle),
                      cy + radius * std::sin(angle));
    };

    // ── Background grid ────────────────────────────────────────────────
    const ImU32 gridColor = IM_COL32(60, 60, 70, 255);
    const ImU32 gridFill  = IM_COL32(30, 30, 38, 255);

    for (int level = 5; level >= 1; --level) {
        float lr = r * level / 5.0f;
        ImVec2 pts[5];
        for (int i = 0; i < 5; ++i) pts[i] = pentagonVertex(i, lr);

        if (level == 5) {
            // Outermost: filled
            dl->AddConvexPolyFilled(pts, 5, gridFill);
        }
        dl->AddPolyline(pts, 5, gridColor, ImDrawFlags_Closed, 1.0f);
    }

    // ── Axis lines ─────────────────────────────────────────────────────
    for (int i = 0; i < 5; ++i) {
        ImVec2 outer = pentagonVertex(i, r);
        dl->AddLine(ImVec2(cx, cy), outer, gridColor, 1.0f);
    }

    // ── Data polygon ───────────────────────────────────────────────────
    ImVec2 dataPts[5];
    for (int i = 0; i < 5; ++i) {
        float v = std::clamp(values[i], 0.0f, 1.0f);
        dataPts[i] = pentagonVertex(i, r * v);
    }

    // Semi-transparent fill
    dl->AddConvexPolyFilled(dataPts, 5, IM_COL32(0, 180, 220, 100));
    // Bright outline
    dl->AddPolyline(dataPts, 5, IM_COL32(0, 220, 255, 255), ImDrawFlags_Closed, 2.0f);

    // Data point dots
    for (int i = 0; i < 5; ++i) {
        dl->AddCircleFilled(dataPts[i], 3.5f, IM_COL32(0, 220, 255, 255));
    }

    // ── Labels ─────────────────────────────────────────────────────────
    for (int i = 0; i < 5; ++i) {
        ImVec2 outer = pentagonVertex(i, r * 1.15f);
        ImVec2 labelSize = ImGui::CalcTextSize(labels[i]);
        ImVec2 labelPos(outer.x - labelSize.x * 0.5f, outer.y - labelSize.y * 0.5f);
        dl->AddText(labelPos, IM_COL32(200, 200, 210, 255), labels[i]);
    }

    // ── Title ──────────────────────────────────────────────────────────
    ImVec2 titleSize = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(cx - titleSize.x * 0.5f, cursor.y),
                IM_COL32(255, 255, 255, 255), title);

    // ── Value labels at each vertex ────────────────────────────────────
    for (int i = 0; i < 5; ++i) {
        float v = values[i];
        ImVec2 pt = pentagonVertex(i, r * 1.05f);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", v);
        ImVec2 vs = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(pt.x - vs.x * 0.5f, pt.y - vs.y),
                    IM_COL32(160, 220, 240, 255), buf);
    }
}

// ---------------------------------------------------------------------------
// SeverityBadge — color-coded text
// ---------------------------------------------------------------------------
void SeverityBadge(const char* label, double value, double warnAt, double dangerAt) {
    ImVec4 color;
    if (value >= dangerAt) {
        color = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);   // Red
    } else if (value >= warnAt) {
        color = ImVec4(1.0f, 0.65f, 0.15f, 1.0f);    // Orange
    } else {
        color = ImVec4(0.30f, 0.95f, 0.35f, 1.0f);   // Green
    }
    ImGui::TextColored(color, "%s: %.1f", label, value);
}

// ---------------------------------------------------------------------------
// HelpMarker — (?) icon with hover tooltip
// ---------------------------------------------------------------------------
void HelpMarker(const char* description) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(description);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------------------
// SliderWithHelp — slider + auto-placed (?) tooltip
// ---------------------------------------------------------------------------
bool SliderWithHelp(const char* label, float* value, float min, float max,
                    const char* helpText, const char* format) {
    bool changed = ImGui::SliderFloat(label, value, min, max, format);
    HelpMarker(helpText);
    return changed;
}

// ---------------------------------------------------------------------------
// ToggleButton — colored on/off button
// ---------------------------------------------------------------------------
bool ToggleButton(const char* label, bool* state, const ImVec2& size) {
    bool clicked = false;
    if (*state) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f, 0.45f, 0.10f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.35f, 0.15f, 0.15f, 1.0f));
    }

    if (ImGui::Button(label, size)) {
        *state = !*state;
        clicked = true;
    }
    ImGui::PopStyleColor(3);
    return clicked;
}

// ---------------------------------------------------------------------------
// LatencyDisplay — colored latency readout
// ---------------------------------------------------------------------------
void LatencyDisplay(double latencyUs, double goodThreshold, double warnThreshold) {
    ImVec4 color;
    const char* status;
    if (latencyUs < goodThreshold) {
        color  = ImVec4(0.30f, 0.95f, 0.35f, 1.0f);  // Green — excellent
        status = "Excellent";
    } else if (latencyUs < warnThreshold) {
        color  = ImVec4(1.0f, 0.85f, 0.20f, 1.0f);   // Yellow — acceptable
        status = "Acceptable";
    } else {
        color  = ImVec4(1.0f, 0.30f, 0.30f, 1.0f);   // Red — high
        status = "High";
    }
    ImGui::TextColored(color, "%.1f μs (%s)", latencyUs, status);
}

} // namespace aether::widgets
