#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <format>
#include <source_location>

namespace aether {

enum class LogLevel { Debug, Info, Warning, Error };

class Logger {
public:
    static Logger& instance();

    void setLogFile(const std::string& path);
    void setMinLevel(LogLevel level);

    template<typename... Args>
    void log(LogLevel lvl, std::format_string<Args...> fmt, Args&&... args) {
        if (lvl < m_minLevel) return;
        std::lock_guard lock(m_mutex);
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        write(lvl, msg);
    }

    // Convenience
    template<typename... Args> void debug(std::format_string<Args...> f, Args&&... a) { log(LogLevel::Debug, f, std::forward<Args>(a)...); }
    template<typename... Args> void info (std::format_string<Args...> f, Args&&... a) { log(LogLevel::Info,  f, std::forward<Args>(a)...); }
    template<typename... Args> void warn (std::format_string<Args...> f, Args&&... a) { log(LogLevel::Warning,f,std::forward<Args>(a)...); }
    template<typename... Args> void error(std::format_string<Args...> f, Args&&... a) { log(LogLevel::Error, f, std::forward<Args>(a)...); }

private:
    Logger() = default;
    void write(LogLevel lvl, const std::string& msg);

    LogLevel m_minLevel = LogLevel::Debug;
    std::mutex m_mutex;
    std::ofstream m_file;
};

// Global convenience macros
#define LOG_DEBUG(...) aether::Logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...)  aether::Logger::instance().info(__VA_ARGS__)
#define LOG_WARN(...)  aether::Logger::instance().warn(__VA_ARGS__)
#define LOG_ERROR(...) aether::Logger::instance().error(__VA_ARGS__)

} // namespace aether
