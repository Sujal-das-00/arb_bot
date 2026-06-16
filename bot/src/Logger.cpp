#include "Logger.h"
#include <chrono>
#include <filesystem>
#include <iomanip>

namespace fs = std::filesystem;

namespace {

// Local calendar date as "YYYY-MM-DD".
std::string today_string(std::time_t time) {
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d");
    return oss.str();
}

} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::open_for_date(const std::string& date) {
    if (file_.is_open()) {
        file_.close();
    }

    std::string log_file = log_dir_ + "/bot_" + date + ".log";
    file_.open(log_file, std::ios::app);
    current_date_ = date;

    if (file_) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        file_ << "[LOG] Opened at " << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S") << "\n";
        file_.flush();
    }
}

void Logger::init(const std::string& log_dir) {
    std::lock_guard<std::mutex> lock(mutex_);

    log_dir_ = log_dir;
    fs::create_directories(log_dir);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    // One file per calendar date; multiple runs on the same day append to it.
    open_for_date(today_string(time));
}

void Logger::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    // Roll over to a new file when the calendar date changes (e.g. bot left
    // running past midnight), so each day's logs land in their own file.
    std::string date = today_string(time);
    if (!log_dir_.empty() && date != current_date_) {
        open_for_date(date);
    }

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
