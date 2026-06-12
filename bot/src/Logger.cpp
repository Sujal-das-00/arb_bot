#include "Logger.h"
#include <chrono>
#include <filesystem>
#include <iomanip>

namespace fs = std::filesystem;

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const std::string& log_dir) {
    std::lock_guard<std::mutex> lock(mutex_);

    fs::create_directories(log_dir);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S");

    std::string log_file = log_dir + "/bot_" + oss.str() + ".log";
    file_.open(log_file, std::ios::app);

    if (file_) {
        file_ << "[LOG] Initialized at " << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S") << "\n";
        file_.flush();
    }
}

void Logger::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << "[" << std::put_time(std::localtime(&time), "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count() << "] " << message;

    std::string timestamped = oss.str();

    std::cout << timestamped << "\n";

    if (file_) {
        file_ << timestamped << "\n";
        file_.flush();
    }
}

void Logger::info(const std::string& message) {
    log("[INFO] " + message);
}

void Logger::warn(const std::string& message) {
    log("[WARN] " + message);
}

void Logger::error(const std::string& message) {
    log("[ERROR] " + message);
}

Logger::~Logger() {
    if (file_) {
        file_.close();
    }
}
