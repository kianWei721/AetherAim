// ============================================================================
// ProfileManager.cpp — Implementation
// ============================================================================

#include "ProfileManager.hpp"
#include "core/InputProcessor.hpp"
#include "core/OneEuroFilter.hpp"   // for betaFromSeverity
#include "utils/Logger.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace aether {

// ---------------------------------------------------------------------------
// Helper: ISO 8601 timestamp string
// ---------------------------------------------------------------------------
static std::string isoNow() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);  // MSVC-safe
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
ProfileManager::ProfileManager(const std::string& profileDir)
    : m_profileDir(profileDir)
{
    // Ensure profile directory exists
    std::error_code ec;
    std::filesystem::create_directories(profileDir, ec);
    if (ec) {
        LOG_WARN("Cannot create profile directory '{}': {}", profileDir, ec.message());
    }

    // Pre-allocate sample buffer to avoid reallocation in hot path
    m_samples.reserve(MAX_SAMPLES);
}

ProfileManager::~ProfileManager() {
    // Best-effort save on destruction
    if (m_sessionActive) {
        endSession();
    }
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------
bool ProfileManager::loadProfile(const std::string& name) {
    std::string path = m_profileDir + "/" + name + ".json";
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_WARN("Profile '{}' not found at '{}'. Creating blank.", name, path);
        m_active = createBlank(name, "unknown");
        return false;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(f);
        m_active = j.get<UserAbilityProfile>();
        m_active.lastUsed = isoNow();
        LOG_INFO("Profile '{}' loaded (game={}, tremor={:.1f}, overshoot={:.1f})",
                 m_active.name, m_active.game,
                 m_active.tremorSeverity, m_active.overshootTendency);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Profile parse error: {}", e.what());
        m_active = createBlank(name, "unknown");
        return false;
    }
}

bool ProfileManager::saveProfile() {
    return saveProfileAs(m_active.name);
}

bool ProfileManager::saveProfileAs(const std::string& name) {
    try {
        m_active.name = name;
        m_active.lastUsed = isoNow();

        std::string path = m_profileDir + "/" + name + ".json";
        nlohmann::json j = m_active;

        // Atomic write: .tmp → rename
        std::string tmpPath = path + ".tmp";
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            LOG_ERROR("Cannot open '{}' for writing", tmpPath);
            return false;
        }
        f << j.dump(4) << '\n';
        f.close();

        std::error_code ec;
        std::filesystem::rename(tmpPath, path, ec);
        if (ec) {
            LOG_ERROR("Profile rename failed: {}", ec.message());
            std::filesystem::remove(tmpPath);
            return false;
        }

        LOG_INFO("Profile '{}' saved", name);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Profile save error: {}", e.what());
        return false;
    }
}

std::vector<std::string> ProfileManager::listProfiles() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_profileDir, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            names.push_back(entry.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool ProfileManager::deleteProfile(const std::string& name) {
    std::string path = m_profileDir + "/" + name + ".json";
    std::error_code ec;
    bool ok = std::filesystem::remove(path, ec);
    if (ok) {
        LOG_INFO("Profile '{}' deleted", name);
    }
    return ok;
}

UserAbilityProfile ProfileManager::createBlank(const std::string& name,
                                                const std::string& game) {
    UserAbilityProfile p;
    p.name    = name;
    p.game    = game;
    p.created = isoNow();
    p.lastUsed = p.created;

    // Sensible defaults for a new user
    p.preferredDPI  = 800;
    p.preferredHz   = 1000.0;
    p.preferredSensitivity = 1.0;

    LOG_DEBUG("Blank profile '{}' created for game '{}'", name, game);
    return p;
}

// ---------------------------------------------------------------------------
// Real-time stats accumulation (called from hook callback)
// ---------------------------------------------------------------------------
void ProfileManager::pushSample(const DeltaSample& sample) {
    std::lock_guard lock(m_sampleMutex);

    m_samples.push_back(sample);

    // Ring-buffer eviction: drop oldest samples when full.
    // We keep MAX_SAMPLES worth of data (~6s at 1000 Hz).
    if (m_samples.size() > MAX_SAMPLES) {
        size_t excess = m_samples.size() - MAX_SAMPLES;
        m_samples.erase(m_samples.begin(), m_samples.begin() + excess);
    }

    m_active.totalSamples++;
}

// ---------------------------------------------------------------------------
// recomputeParameters — analyze accumulated samples, update profile
//
// Call from GUI thread at ~5-10 Hz. Reads sample buffer under lock.
// ---------------------------------------------------------------------------
void ProfileManager::recomputeParameters() {
    std::vector<DeltaSample> snapshot;
    {
        std::lock_guard lock(m_sampleMutex);
        snapshot = m_samples;  // Copy under lock
    }

    if (snapshot.size() < 50) {
        // Not enough data for meaningful statistics
        return;
    }

    // ── Average speeds ─────────────────────────────────────────────────
    double sumRaw  = 0.0;
    double sumFilt = 0.0;
    double sumLat  = 0.0;
    int    count   = 0;

    for (const auto& s : snapshot) {
        sumRaw  += s.rawSpeed;
        sumFilt += s.filtSpeed;
        count++;
    }

    if (count > 0) {
        m_active.avgRawSpeed      = sumRaw  / count;
        m_active.avgFilteredSpeed = sumFilt / count;
    }

    // ── Tremor frequency (zero-crossing on high-passed X-axis) ────────
    m_active.tremorFreqHz = estimateTremorFrequency();

    // ── Overshoot ratio ────────────────────────────────────────────────
    m_active.overshootRatio = estimateOvershootRatio();

    // ── Fatigue rate ───────────────────────────────────────────────────
    m_active.fatigueRate = estimateFatigueRate();

    // ── Auto-tune filter parameters from assessment ────────────────────
    double sev = m_active.tremorSeverity;
    if (sev < 0.1 && m_active.tremorFreqHz > 2.0) {
        // Tremor detected but severity not set — auto-estimate
        // Map frequency to severity: 2Hz → 1, 8Hz → 7, 12Hz → 10
        sev = std::clamp((m_active.tremorFreqHz - 1.0) * 0.9, 0.0, 10.0);
    }
    m_active.recommendedBeta       = OneEuroFilter::betaFromSeverity(
        static_cast<int>(std::round(sev)));
    m_active.recommendedMinCutoff  = std::clamp(
        0.5 + m_active.tremorFreqHz * 0.15, 0.1, 8.0);
    m_active.recommendedSpeedCoeff = 1.0 + m_active.overshootTendency * 0.5;
    m_active.recommendedDCutoff    = 1.0;  // Derivative cutoff is usually fine at 1 Hz

    m_active.avgLatencyUs = sumLat;

    LOG_DEBUG("Profile recomputed: β={:.1f}, fc={:.2f}Hz, tremor={:.1f}Hz, overshoot={:.2f}",
              m_active.recommendedBeta, m_active.recommendedMinCutoff,
              m_active.tremorFreqHz, m_active.overshootRatio);
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------
void ProfileManager::startSession() {
    m_sessionStart  = std::chrono::steady_clock::now();
    m_sessionActive = true;

    // Clear previous session samples (keep profile metadata)
    {
        std::lock_guard lock(m_sampleMutex);
        m_samples.clear();
    }
    m_active.totalSamples     = 0;
    m_active.avgRawSpeed      = 0.0;
    m_active.avgFilteredSpeed = 0.0;
    m_active.avgLatencyUs     = 0.0;

    LOG_INFO("Session started for profile '{}'", m_active.name);
}

void ProfileManager::endSession() {
    if (!m_sessionActive) return;

    recomputeParameters();  // Final stats update
    saveProfile();          // Always persist on session end

    m_sessionActive = false;
    LOG_INFO("Session ended ({} samples, {:.0f}s)",
             m_active.totalSamples, sessionDurationS());
}

// ---------------------------------------------------------------------------
// applyToProcessor — push profile-derived parameters downstream
// ---------------------------------------------------------------------------
void ProfileManager::applyToProcessor(InputProcessor& processor) {
    // Use auto-tuned parameters if available, otherwise fall back to manual
    double beta = m_active.recommendedBeta;
    double fc   = m_active.recommendedMinCutoff;
    double sc   = m_active.recommendedSpeedCoeff;
    double dc   = m_active.recommendedDCutoff;

    // If no recommendation computed yet, use tremor severity to bootstrap
    if (beta < 0.001 && m_active.tremorSeverity > 0.1) {
        beta = OneEuroFilter::betaFromSeverity(
            static_cast<int>(std::round(m_active.tremorSeverity)));
        fc   = std::clamp(0.5 + m_active.tremorSeverity * 0.2, 0.1, 8.0);
        sc   = 1.0 + m_active.overshootTendency * 0.5;
    }

    InputProcessor::FilterProfile fp;
    fp.type       = FilterType::OneEuro;
    fp.minCutoff  = fc;
    fp.beta       = beta;
    fp.dCutoff    = dc;
    fp.speedCoeff = sc;
    fp.deadzone   = 0;  // Deadzone is a global setting, not per-profile
    fp.maxDelta   = 100;

    processor.applyProfile(fp);

    LOG_DEBUG("Profile applied to processor: β={:.2f}, fc={:.2f}Hz", beta, fc);
}

// ---------------------------------------------------------------------------
// Statistics accessors
// ---------------------------------------------------------------------------
size_t ProfileManager::sampleCount() const {
    std::lock_guard lock(m_sampleMutex);
    return m_samples.size();
}

double ProfileManager::sessionDurationS() const {
    if (!m_sessionActive) return 0.0;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - m_sessionStart).count();
}

std::vector<DeltaSample> ProfileManager::sampleSnapshot(size_t maxCount) const {
    std::lock_guard lock(m_sampleMutex);
    if (m_samples.size() <= maxCount) {
        return m_samples;  // Full copy
    }
    // Return the most recent maxCount samples
    auto start = m_samples.end() - static_cast<ptrdiff_t>(maxCount);
    return std::vector<DeltaSample>(start, m_samples.end());
}

// ============================================================================
// Private: statistical estimators
// ============================================================================

// ---------------------------------------------------------------------------
// estimateTremorFrequency — zero-crossing rate on high-pass filtered signal
//
// High-pass = difference between consecutive samples (removes slow aim motion,
// leaves only high-frequency tremor components).
// Zero-crossing rate / 2 = dominant frequency.
// ---------------------------------------------------------------------------
double ProfileManager::estimateTremorFrequency() const {
    // Read under lock
    std::vector<DeltaSample> snapshot;
    {
        std::lock_guard lock(m_sampleMutex);
        if (m_samples.size() < 50) return 0.0;
        snapshot = m_samples;  // Copy under lock
    }

    int    zeroCrossings = 0;
    double prevHP = 0.0;
    bool   first  = true;

    for (size_t i = 1; i < snapshot.size(); ++i) {
        // Use X-axis for tremor detection (horizontal tremor is most common)
        double hp = snapshot[i].rawDx - snapshot[i - 1].rawDx;

        if (!first && prevHP * hp < 0.0) {
            zeroCrossings++;
        }
        prevHP = hp;
        first  = false;
    }

    double duration = snapshot.back().timestamp - snapshot.front().timestamp;
    if (duration <= 0.0) return 0.0;

    // Frequency = crossings / (2 * duration)
    return static_cast<double>(zeroCrossings) / (2.0 * duration);
}

// ---------------------------------------------------------------------------
// estimateOvershootRatio — detect direction reversals after fast movements
//
// An "overshoot" is defined as: a rapid acceleration followed immediately
// by a deceleration in the same direction, where the deceleration exceeds
// a threshold (indicating the user went past the target and corrected).
// ---------------------------------------------------------------------------
double ProfileManager::estimateOvershootRatio() const {
    std::vector<DeltaSample> snapshot;
    {
        std::lock_guard lock(m_sampleMutex);
        if (m_samples.size() < 100) return 0.0;
        snapshot = m_samples;
    }

    int overshoots = 0;
    int movements  = 0;

    for (size_t i = 3; i < snapshot.size(); ++i) {
        double s0 = std::abs(snapshot[i - 3].rawDx);
        double s1 = std::abs(snapshot[i - 2].rawDx);
        double s2 = std::abs(snapshot[i - 1].rawDx);
        double s3 = std::abs(snapshot[i].rawDx);

        // Detect a "burst": speed increases then decreases
        if (s1 > 5.0) {  // Only track meaningful movements (>5 px/sample)
            movements++;
            // Pattern: low → high → medium → low  (accel → overshoot → correction)
            // Simplified: s0 < s1 and s2 < s1 and s3 < s2
            if (s1 > s0 && s2 < s1 && s3 < s2) {
                overshoots++;
            }
        }
    }

    return movements > 0
        ? std::clamp(static_cast<double>(overshoots) / movements, 0.0, 1.0)
        : 0.0;
}

// ---------------------------------------------------------------------------
// estimateFatigueRate — measure speed decay over the session
//
// Compares average speed in the first 20% of samples vs last 20%.
// A negative slope indicates fatigue (player slowing down).
// ---------------------------------------------------------------------------
double ProfileManager::estimateFatigueRate() const {
    std::vector<DeltaSample> snapshot;
    {
        std::lock_guard lock(m_sampleMutex);
        if (m_samples.size() < 200) return 0.0;
        snapshot = m_samples;
    }

    size_t n        = snapshot.size();
    size_t segSize  = n / 5;  // 20% segments
    if (segSize < 20) return 0.0;

    // First segment average speed
    double firstAvg = 0.0;
    for (size_t i = 0; i < segSize; ++i) {
        firstAvg += snapshot[i].rawSpeed;
    }
    firstAvg /= static_cast<double>(segSize);

    // Last segment average speed
    double lastAvg = 0.0;
    for (size_t i = n - segSize; i < n; ++i) {
        lastAvg += snapshot[i].rawSpeed;
    }
    lastAvg /= static_cast<double>(segSize);

    if (firstAvg < 1.0) return 0.0;  // Not enough movement to judge

    // Fatigue = 1.0 - (lastSpeed / firstSpeed), clamped to [0, 10]
    double fatigueRaw = (1.0 - lastAvg / firstAvg) * 10.0;
    return std::clamp(fatigueRaw, 0.0, 10.0);
}

} // namespace aether
