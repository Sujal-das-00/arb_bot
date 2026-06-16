#pragma once

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <memory>
#include <mutex>

class Logger {
public:
    static Logger& instance();

    void init(const std::string& log_dir);
    void log(const std::string& message);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

    template <typename... Args>
    void logf(const std::string& format, Args... args) {
        std::ostringstream oss;
        format_impl(oss, format, args...);
        log(oss.str());
    }

private:
    Logger() = default;
    ~Logger();

    // Opens (appending) the log file for the given date, e.g.
    // "<log_dir>/bot_2026-06-16.log". Caller must hold mutex_.
    void open_for_date(const std::string& date);

    std::ofstream file_;
    std::mutex mutex_;
    std::string log_dir_;     // where log files live, set in init()
    std::string current_date_; // "%Y-%m-%d" of the currently open file

    template <typename T>
    void format_impl(std::ostringstream& oss, const std::string& format, T value) {
        size_t pos = format.find("{}");
        if (pos != std::string::npos) {
            oss << format.substr(0, pos) << value << format.substr(pos + 2);
        } else {
            oss << format;
        }
    }

    template <typename T, typename... Args>
    void format_impl(std::ostringstream& oss, const std::string& format, T value, Args... args) {
        size_t pos = format.find("{}");
        if (pos != std::string::npos) {
            oss << format.substr(0, pos) << value;
            format_impl(oss, format.substr(pos + 2), args...);
        }
    }
};
