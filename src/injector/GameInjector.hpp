#pragma once
// ============================================================================
// GameInjector.hpp — DLL Injection / Ejection Manager
//
// Injects AetherAimHook.dll into a target game process using the
// standard CreateRemoteThread + LoadLibraryW technique.
//
// Supported games (pre-populated process names):
//   cs2.exe, valorant.exe, overwatch.exe, r5apex.exe, cod.exe
//
// ANTI-CHEAT WARNING:
//   Injection will fail or be blocked by kernel-level anti-cheat.
//   Vanguard (Valorant) blocks OpenProcess entirely.
//   BattlEye / EAC may flag the injection as suspicious.
//   Use only in environments where input modification is permitted.
// ============================================================================

#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

namespace aether {

// ── Per-game process info ─────────────────────────────────────────────
struct GameInfo {
    std::wstring name;          // "Counter-Strike 2"
    std::wstring processName;   // "cs2.exe"
    std::wstring displayIcon;   // future: icon resource identifier
};

// ── Injection state for a single game ─────────────────────────────────
enum class InjectionState {
    NotRunning,      // Game process not detected
    Running,         // Game running, not injected
    Injected,        // DLL successfully injected
    Blocked,         // Injection blocked (anti-cheat, permissions)
    Error            // Injection failed for other reasons
};

struct GameStatus {
    GameInfo        info;
    DWORD           pid = 0;
    InjectionState  state = InjectionState::NotRunning;
    uint64_t        totalEvents = 0;
    uint64_t        filteredEvents = 0;
    double          avgLatencyNs = 0.0;
};

// ── Callbacks ─────────────────────────────────────────────────────────
using StatusCallback = std::function<void(const GameStatus&)>;

// ============================================================================
// GameInjector
// ============================================================================
class GameInjector {
public:
    GameInjector();
    ~GameInjector();

    // Non-copyable
    GameInjector(const GameInjector&)            = delete;
    GameInjector& operator=(const GameInjector&) = delete;

    // ── Static game database ───────────────────────────────────────────
    static const std::vector<GameInfo>& supportedGames();

    // ── Scan for running games ────────────────────────────────────────
    // Returns a list of running supported games with their status.
    std::vector<GameStatus> scanGames();

    // ── Inject DLL into a specific process ────────────────────────────
    // Returns true if injection succeeded (or was already injected).
    bool inject(DWORD pid);

    // ── Eject DLL from a specific process ─────────────────────────────
    bool eject(DWORD pid);

    // ── Check if a process has our DLL loaded ─────────────────────────
    bool isDllLoaded(DWORD pid) const;

    // ── Get the DLL path (resolved at construction time) ──────────────
    const std::wstring& dllPath() const { return m_dllPath; }

    // ── Shared memory management ───────────────────────────────────────
    bool createSharedMemory();
    void destroySharedMemory();
    bool hasSharedMemory() const { return m_hSharedMem != nullptr; }

    // ── Set filter enabled state in shared memory ─────────────────────
    void setFilterEnabled(bool enabled);

    // ── Status callback (called when scanGames detects changes) ───────
    void setStatusCallback(StatusCallback cb) { m_statusCB = std::move(cb); }

private:
    // Find process ID by name
    DWORD findProcess(const std::wstring& processName) const;

    // Inject into a process with specified access rights
    bool injectIntoProcess(HANDLE hProcess, const std::wstring& dllPath);

    // Eject from a process
    bool ejectFromProcess(HANDLE hProcess, DWORD pid);

    std::wstring    m_dllPath;
    HANDLE          m_hSharedMem   = nullptr;
    LPVOID          m_pSharedView  = nullptr;   // Persistent mapped view
    StatusCallback  m_statusCB;

    // Track injected processes
    std::mutex         m_mutex;
    std::vector<DWORD> m_injectedPids;
};

} // namespace aether
