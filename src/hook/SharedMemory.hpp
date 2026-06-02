#pragma once
// ============================================================================
// SharedMemory.hpp — Inter-Process Communication Protocol
//
// A file-mapping-backed control block shared between AetherAim.exe (writer)
// and AetherAimHook.dll (reader, injected into the game process).
//
// Layout (single page, 4096 bytes):
//   [0x000] HookControlBlock   — filter parameters + control flags
//   [0x200] StatsRingBuffer    — per-event statistics (DLL → main process)
//
// Synchronization:
//   - Control params: relaxed atomic reads. The DLL reads params on each
//     GetRawInputData call. If the user changes params in the GUI, the
//     new values appear within one mouse event (~1ms).
//   - Stats feedback: atomic counter. The DLL writes stats; the GUI reads
//     them periodically (~10 Hz) for the "Games" tab display.
// ============================================================================

#include <cstdint>
#include <atomic>

namespace aether::ipc {

// ── Filter type enumeration (keep in sync with core/MouseHookManager.hpp) ─
enum class IPC_FilterType : int32_t {
    OneEuro = 0,
    EMA     = 1,
    Kalman  = 2,
    None    = 3
};

// ── OneEuro parameters (packed for shared memory) ───────────────────────
struct alignas(32) IPC_OneEuroParams {
    double minCutoff  = 1.2;
    double beta       = 0.0;
    double dCutoff    = 1.0;
    double speedCoeff = 1.0;
};

struct alignas(32) IPC_EMAParams {
    double alpha = 0.6;
};

struct alignas(32) IPC_KalmanParams {
    double processNoise     = 0.01;
    double measurementNoise = 0.1;
};

// ── Main control block (written by AetherAim.exe, read by DLL) ──────────
struct alignas(64) HookControlBlock {
    // Magic number for DLL validation: must be 0xAETHER42
    uint32_t magic         = 0;
    uint32_t version       = 1;

    // Filter enable/disable (atomic — DLL reads without lock)
    std::atomic<int32_t> filterEnabled{0};

    // Active filter type
    std::atomic<int32_t> filterType{0};

    // Per-filter parameters
    IPC_OneEuroParams  oneEuro;
    IPC_EMAParams      ema;
    IPC_KalmanParams   kalman;

    // General settings
    std::atomic<int32_t> deadzonePx{0};
    std::atomic<int32_t> maxDeltaPx{100};
    std::atomic<int32_t> subPixelAccum{1};

    // Reserved for future use
    uint8_t _padding[64];
};

static_assert(sizeof(HookControlBlock) <= 512,
    "Control block must fit in 512 bytes for cache-line friendliness");

// ── Statistics ring buffer entry (written by DLL, read by main process) ─
struct alignas(32) IPC_StatsEntry {
    std::atomic<uint64_t> totalEvents{0};
    std::atomic<uint64_t> filteredEvents{0};
    std::atomic<double>   avgLatencyNs{0.0};   // nanoseconds per hook call
    std::atomic<double>   avgRawSpeed{0.0};    // px/s
    std::atomic<double>   avgFiltSpeed{0.0};   // px/s
    std::atomic<double>   lastUpdateTime{0.0}; // QPC seconds
};

// ── Shared memory layout ────────────────────────────────────────────────
struct SharedMemoryLayout {
    HookControlBlock control;
    IPC_StatsEntry   stats;
};

// Total size: ~640 bytes (well within one 4KB page)
static_assert(sizeof(SharedMemoryLayout) <= 4096,
    "Shared memory must fit in one page");

// ── Named file mapping object name ─────────────────────────────────────
// Both the main process and DLL use this name to open the same mapping.
constexpr const wchar_t* SHARED_MEMORY_NAME = L"Global\\AetherAim_SharedMemory";

// Validation magic
constexpr uint32_t SHARED_MEMORY_MAGIC = 0xAETHER42;

} // namespace aether::ipc
