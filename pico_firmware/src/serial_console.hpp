#pragma once
// ============================================================================
// Serial Console — Runtime parameter adjustment via USB serial (115200 baud)
//
// Commands:
//   fc <float>    — set min cutoff frequency (Hz)
//   beta <float>  — set beta (speed adaptation)
//   dc <float>    — set derivative cutoff (Hz)
//   sc <float>    — set speed coefficient
//   dz <int>      — set deadzone (pixels)
//   md <int>      — set max delta clamp (pixels)
//   status        — print current parameters
//   reset         — reset filter state
//   help          — show available commands
// ============================================================================

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace aether::console {

// Buffer for incoming serial data
static char  g_cmdBuf[64];
static int   g_cmdLen = 0;

// ── Parse helpers ──────────────────────────────────────────────────────

static float parseFloat(const char* s, float fallback) {
    char* end = nullptr;
    float v = strtof(s, &end);
    return (end != s) ? v : fallback;
}

static int parseInt(const char* s, int fallback) {
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    return (end != s) ? static_cast<int>(v) : fallback;
}

// ── Command handlers ───────────────────────────────────────────────────

template<typename Filter>
static bool processCommand(const char* cmd, const char* arg,
                           Filter& filter,
                           float& minCutoff, float& beta, float& dCutoff,
                           float& speedCoeff, int& deadzone, int& maxDelta) {
    if (strcmp(cmd, "fc") == 0) {
        minCutoff = std::max(parseFloat(arg, minCutoff), 0.01f);
        filter.setParams(minCutoff, beta, dCutoff, speedCoeff);
        printf("OK  min_cutoff = %.2f Hz\n", minCutoff);
        return true;
    }
    if (strcmp(cmd, "beta") == 0) {
        beta = std::max(parseFloat(arg, beta), 0.0f);
        filter.setParams(minCutoff, beta, dCutoff, speedCoeff);
        printf("OK  beta = %.2f\n", beta);
        return true;
    }
    if (strcmp(cmd, "dc") == 0) {
        dCutoff = std::max(parseFloat(arg, dCutoff), 0.01f);
        filter.setParams(minCutoff, beta, dCutoff, speedCoeff);
        printf("OK  d_cutoff = %.2f Hz\n", dCutoff);
        return true;
    }
    if (strcmp(cmd, "sc") == 0) {
        speedCoeff = std::max(parseFloat(arg, speedCoeff), 0.01f);
        filter.setParams(minCutoff, beta, dCutoff, speedCoeff);
        printf("OK  speed_coeff = %.2f\n", speedCoeff);
        return true;
    }
    if (strcmp(cmd, "dz") == 0) {
        deadzone = std::max(parseInt(arg, deadzone), 0);
        printf("OK  deadzone = %d px\n", deadzone);
        return true;
    }
    if (strcmp(cmd, "md") == 0) {
        maxDelta = std::max(parseInt(arg, maxDelta), 10);
        printf("OK  max_delta = %d px\n", maxDelta);
        return true;
    }
    if (strcmp(cmd, "reset") == 0) {
        filter.reset();
        printf("OK  filter reset\n");
        return true;
    }
    if (strcmp(cmd, "status") == 0) {
        printf("\n");
        printf("  Filter:       OneEuro\n");
        printf("  Min Cutoff:   %.2f Hz\n", minCutoff);
        printf("  Beta:         %.2f\n", beta);
        printf("  D-Cutoff:     %.2f Hz\n", dCutoff);
        printf("  Speed Coeff:  %.2f\n", speedCoeff);
        printf("  Deadzone:     %d px\n", deadzone);
        printf("  Max Delta:    %d px\n", maxDelta);
        printf("\n");
        return true;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        printf("\n");
        printf("  AetherAim Pico — Serial Commands\n");
        printf("  ────────────────────────────────\n");
        printf("  fc <float>    Min cutoff (Hz)       [%.2f]\n", minCutoff);
        printf("  beta <float>  Speed adaptation      [%.2f]\n", beta);
        printf("  dc <float>    Derivative cutoff     [%.2f]\n", dCutoff);
        printf("  sc <float>    Speed coefficient     [%.2f]\n", speedCoeff);
        printf("  dz <int>      Deadzone (px)         [%d]\n", deadzone);
        printf("  md <int>      Max delta clamp (px)  [%d]\n", maxDelta);
        printf("  status        Print current params\n");
        printf("  reset         Reset filter state\n");
        printf("  help          Show this message\n");
        printf("\n");
        return true;
    }

    printf("ERR  Unknown command: '%s'  (type 'help' for list)\n", cmd);
    return false;
}

// ── Poll serial input (call in main loop, non-blocking) ────────────────

template<typename Filter>
inline void pollSerial(Filter& filter,
                       float& fc, float& b, float& dc, float& sc,
                       int& dz, int& md) {
    int ch = getchar_timeout_us(0);  // Non-blocking
    if (ch == PICO_ERROR_TIMEOUT) return;

    putchar_raw(ch);  // Echo

    if (ch == '\r' || ch == '\n') {
        // Command complete
        if (g_cmdLen > 0) {
            g_cmdBuf[g_cmdLen] = '\0';

            // Split into command + argument
            char* space = strchr(g_cmdBuf, ' ');
            const char* cmd = g_cmdBuf;
            const char* arg = "";

            if (space) {
                *space = '\0';
                arg = space + 1;
                // Skip multiple spaces
                while (*arg == ' ') arg++;
            }

            if (strlen(cmd) > 0) {
                processCommand(cmd, arg, filter, fc, b, dc, sc, dz, md);
            }

            g_cmdLen = 0;
        }
        printf("\n> ");  // Prompt
    } else if (ch == '\b' || ch == 127) {
        // Backspace
        if (g_cmdLen > 0) {
            g_cmdLen--;
            printf("\b \b");  // Erase character in terminal
        }
    } else if (g_cmdLen < static_cast<int>(sizeof(g_cmdBuf)) - 1) {
        g_cmdBuf[g_cmdLen++] = static_cast<char>(ch);
    }
}

} // namespace aether::console
