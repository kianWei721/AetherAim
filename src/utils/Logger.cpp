#include "Logger.hpp"
#include <iostream>
#include <chrono>
#include <iomanip>

namespace aether {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::setLogFile(const std::string& path) {
    std::lock_guard lock(m_mutex);
    if (m_file.is_open()) m_file.close();
    m_file.open(path, std::ios::app);
}

void Logger::setMinLevel(LogLevel level) {
    m_minLevel = level;
}

void Logger::write(LogLevel lvl, const std::string& msg) {
    const char* prefixes[] = {"[DBG]", "[INF]", "[WRN]", "[ERR]"};
    int idx = static_cast<int>(lvl);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);

    // Console output
    std::cout << prefixes[idx] << " " << msg << '\n';

    // File output
    if (m_file.is_open()) {
        m_file << prefixes[idx] << " " << msg << '\n';
        m_file.flush();
    }
}

} // namespace aether
