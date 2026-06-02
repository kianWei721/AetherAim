// ============================================================================
// GameInjector.cpp — Implementation
// ============================================================================

#include "GameInjector.hpp"
#include "hook/SharedMemory.hpp"
#include "utils/Logger.hpp"

#include <TlHelp32.h>
#include <Psapi.h>
#include <algorithm>
#include <cstring>

namespace aether {

// ============================================================================
// Supported games database
// ============================================================================
const std::vector<GameInfo>& GameInjector::supportedGames() {
    static const std::vector<GameInfo> games = {
        { L"Counter-Strike 2",     L"cs2.exe" },
        { L"Valorant",             L"valorant.exe" },
        { L"Overwatch 2",          L"overwatch.exe" },
        { L"Apex Legends",         L"r5apex.exe" },
        { L"Call of Duty",         L"cod.exe" },
        { L"Rainbow Six Siege",    L"RainbowSix.exe" },
        { L"Quake Champions",      L"QuakeChampions.exe" },
        { L"Splitgate 2",          L"splitgate2.exe" },
    };
    return games;
}

// ============================================================================
// Construction
// ============================================================================
GameInjector::GameInjector() {
    // Resolve the full path to AetherAimHook.dll (same directory as the exe)
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    // Strip the executable name
    wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    }
    m_dllPath = std::wstring(exeDir) + L"AetherAimHook.dll";

    LOG_INFO("GameInjector: DLL path = {}",
             std::string(m_dllPath.begin(), m_dllPath.end()));
}

GameInjector::~GameInjector() {
    // Eject from all injected processes
    for (DWORD pid : m_injectedPids) {
        eject(pid);
    }
    destroySharedMemory();
}

// ============================================================================
// scanGames — enumerate running processes, match against game database
// ============================================================================
std::vector<GameStatus> GameInjector::scanGames() {
    std::vector<GameStatus> result;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateToolhelp32Snapshot failed: {}", GetLastError());
        return result;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            std::wstring procName(pe.szExeFile);

            // Check against supported games
            for (const auto& game : supportedGames()) {
                if (_wcsicmp(procName.c_str(), game.processName.c_str()) == 0) {
                    GameStatus status;
                    status.info = game;
                    status.pid  = pe.th32ProcessID;

                    // Check if already injected
                    if (isDllLoaded(pe.th32ProcessID)) {
                        status.state = InjectionState::Injected;
                    } else {
                        status.state = InjectionState::Running;
                    }

                    // Read stats from shared memory if available
                    if (m_pSharedView && status.state == InjectionState::Injected) {
                        auto* layout = static_cast<ipc::SharedMemoryLayout*>(m_pSharedView);
                        auto& stats = layout->stats;
                        status.totalEvents    = stats.totalEvents.load(std::memory_order_relaxed);
                        status.filteredEvents = stats.filteredEvents.load(std::memory_order_relaxed);
                        status.avgLatencyNs   = stats.avgLatencyNs.load(std::memory_order_relaxed);
                    }

                    result.push_back(status);
                    break;
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return result;
}

// ============================================================================
// inject — inject DLL into target process
// ============================================================================
bool GameInjector::inject(DWORD pid) {
    // Check if already injected
    if (isDllLoaded(pid)) {
        LOG_INFO("DLL already loaded in PID {}", pid);
        std::lock_guard lock(m_mutex);
        if (std::find(m_injectedPids.begin(), m_injectedPids.end(), pid) == m_injectedPids.end()) {
            m_injectedPids.push_back(pid);
        }
        return true;
    }

    // Ensure shared memory is created before injection
    if (!hasSharedMemory()) {
        createSharedMemory();
    }

    // Open the target process
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_READ |
        PROCESS_VM_WRITE,
        FALSE, pid
    );

    if (!hProcess) {
        DWORD err = GetLastError();
        LOG_ERROR("OpenProcess(PID={}) failed: error {}", pid, err);
        if (err == 5) {  // ACCESS_DENIED
            LOG_WARN("Access denied — likely blocked by anti-cheat (Vanguard/EAC/BattlEye)");
        }
        return false;
    }

    bool success = injectIntoProcess(hProcess, m_dllPath);
    CloseHandle(hProcess);

    if (success) {
        std::lock_guard lock(m_mutex);
        m_injectedPids.push_back(pid);
        LOG_INFO("DLL injected into PID {}", pid);
    }

    return success;
}

bool GameInjector::injectIntoProcess(HANDLE hProcess, const std::wstring& dllPath) {
    // Verify file exists
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_ERROR("Hook DLL not found: {}",
                  std::string(dllPath.begin(), dllPath.end()));
        return false;
    }

    // Allocate memory for the DLL path in the target process
    size_t pathSizeBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID pRemoteMem = VirtualAllocEx(
        hProcess, nullptr, pathSizeBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    );

    if (!pRemoteMem) {
        LOG_ERROR("VirtualAllocEx failed: {}", GetLastError());
        return false;
    }

    // Write the DLL path
    if (!WriteProcessMemory(hProcess, pRemoteMem, dllPath.c_str(),
                            pathSizeBytes, nullptr)) {
        LOG_ERROR("WriteProcessMemory failed: {}", GetLastError());
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return false;
    }

    // Get LoadLibraryW address (kernel32.dll is at the same base address
    // in all processes on the same Windows version)
    auto pLoadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    if (!pLoadLibraryW) {
        LOG_ERROR("GetProcAddress(LoadLibraryW) failed");
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return false;
    }

    // Create remote thread to call LoadLibraryW(dllPath)
    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0,
        pLoadLibraryW,
        pRemoteMem,
        0, nullptr
    );

    if (!hThread) {
        LOG_ERROR("CreateRemoteThread failed: {}", GetLastError());
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
        return false;
    }

    // Wait for LoadLibraryW to complete (max 5 seconds)
    DWORD waitResult = WaitForSingleObject(hThread, 5000);

    // Get the DLL's module handle (return value of LoadLibraryW)
    DWORD exitCode = 0;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeThread(hThread, &exitCode);
    }

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);

    if (waitResult != WAIT_OBJECT_0 || exitCode == 0) {
        LOG_ERROR("LoadLibraryW in target process failed: exitCode=0x{:X}, waitResult={}",
                  exitCode, waitResult);
        return false;
    }

    return true;
}

// ============================================================================
// eject — unload DLL from target process
// ============================================================================
bool GameInjector::eject(DWORD pid) {
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_READ,
        FALSE, pid
    );

    if (!hProcess) {
        LOG_ERROR("OpenProcess(PID={}) for eject failed: {}", pid, GetLastError());
        return false;
    }

    bool success = ejectFromProcess(hProcess, pid);
    CloseHandle(hProcess);

    if (success) {
        std::lock_guard lock(m_mutex);
        auto it = std::find(m_injectedPids.begin(), m_injectedPids.end(), pid);
        if (it != m_injectedPids.end()) {
            m_injectedPids.erase(it);
        }
        LOG_INFO("DLL ejected from PID {}", pid);
    }

    return success;
}

bool GameInjector::ejectFromProcess(HANDLE hProcess, DWORD pid) {
    // Find our DLL's module in the target process
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateToolhelp32Snapshot(modules) failed: {}", GetLastError());
        return false;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    HMODULE hOurDll = nullptr;

    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"AetherAimHook.dll") == 0) {
                hOurDll = me.hModule;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);

    if (!hOurDll) {
        LOG_WARN("AetherAimHook.dll not found in PID {}", pid);
        return true;  // Already unloaded — not an error
    }

    // Create remote thread to call FreeLibrary
    auto pFreeLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary"));

    if (!pFreeLibrary) return false;

    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0,
        pFreeLibrary,
        hOurDll,
        0, nullptr
    );

    if (!hThread) {
        LOG_ERROR("CreateRemoteThread(FreeLibrary) failed: {}", GetLastError());
        return false;
    }

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    return true;
}

// ============================================================================
// isDllLoaded — check if our DLL is loaded in a process
// ============================================================================
bool GameInjector::isDllLoaded(DWORD pid) const {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;

    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"AetherAimHook.dll") == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
    return found;
}

// ============================================================================
// Shared memory management
// ============================================================================
bool GameInjector::createSharedMemory() {
    if (m_hSharedMem) return true;

    // Create a named file mapping backed by the system paging file
    m_hSharedMem = CreateFileMappingW(
        INVALID_HANDLE_VALUE,         // paging file
        nullptr,                       // default security
        PAGE_READWRITE,
        0,
        sizeof(ipc::SharedMemoryLayout),
        ipc::SHARED_MEMORY_NAME
    );

    if (!m_hSharedMem) {
        LOG_ERROR("CreateFileMappingW failed: {}", GetLastError());
        return false;
    }

    bool alreadyExists = (GetLastError() == ERROR_ALREADY_EXISTS);

    // Map into our address space (persistent — kept until destroy)
    m_pSharedView = MapViewOfFile(
        m_hSharedMem,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0, 0, 0
    );

    if (!m_pSharedView) {
        LOG_ERROR("MapViewOfFile failed: {}", GetLastError());
        CloseHandle(m_hSharedMem);
        m_hSharedMem = nullptr;
        return false;
    }

    // Initialize the control block (only if we created it)
    if (!alreadyExists) {
        auto* layout = static_cast<ipc::SharedMemoryLayout*>(m_pSharedView);
        ZeroMemory(layout, sizeof(ipc::SharedMemoryLayout));
        layout->control.magic   = ipc::SHARED_MEMORY_MAGIC;
        layout->control.version = 1;
        layout->control.filterEnabled.store(0, std::memory_order_release);
        layout->control.filterType.store(0, std::memory_order_release);
    }

    LOG_INFO("Shared memory {}",
             alreadyExists ? "opened (existing)" : "created");
    return true;
}

void GameInjector::destroySharedMemory() {
    if (m_pSharedView) {
        UnmapViewOfFile(m_pSharedView);
        m_pSharedView = nullptr;
    }
    if (m_hSharedMem) {
        CloseHandle(m_hSharedMem);
        m_hSharedMem = nullptr;
    }
    LOG_DEBUG("Shared memory destroyed");
}

void GameInjector::setFilterEnabled(bool enabled) {
    if (!m_pSharedView) return;
    auto* layout = static_cast<ipc::SharedMemoryLayout*>(m_pSharedView);
    layout->control.filterEnabled.store(enabled ? 1 : 0, std::memory_order_release);
}


} // namespace aether
