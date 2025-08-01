#include "WfbLogger.hpp"

#include <android/log.h>
#include <chrono>
#include <cstdio>

WfbLogger& WfbLogger::getInstance() {
    static WfbLogger instance;
    return instance;
}

void WfbLogger::log(Level level, Category category, const std::string& message) {
    if (!shouldLog(level)) {
        return;
    }

    const char* tag = categoryToTag(category);
    androidLog(level, tag, message);
}



void WfbLogger::debug(Category category, const std::string& message) {
    log(Level::DEBUG, category, message);
}

void WfbLogger::info(Category category, const std::string& message) {
    log(Level::INFO, category, message);
}

void WfbLogger::warn(Category category, const std::string& message) {
    log(Level::WARN, category, message);
}

void WfbLogger::error(Category category, const std::string& message) {
    log(Level::ERROR, category, message);
}

void WfbLogger::setMinLevel(Level level) {
    min_level_ = level;
}

bool WfbLogger::shouldLog(Level level) const {
    return static_cast<int>(level) >= static_cast<int>(min_level_);
}

const char* WfbLogger::levelToString(Level level) {
    switch (level) {
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO";
        case Level::WARN:  return "WARN";
        case Level::ERROR: return "ERROR";
        default:           return "UNKNOWN";
    }
}

const char* WfbLogger::categoryToString(Category category) {
    switch (category) {
        case Category::DEVICE:     return "DEVICE";
        case Category::NETWORK:    return "NETWORK";
        case Category::THREAD:     return "THREAD";
        case Category::ADAPTIVE:   return "ADAPTIVE";
        case Category::JNI:        return "JNI";
        case Category::CONFIG:     return "CONFIG";
        case Category::AGGREGATOR: return "AGGREGATOR";
        case Category::GENERAL:    return "GENERAL";
        default:                   return "UNKNOWN";
    }
}

const char* WfbLogger::categoryToTag(Category category) {
    switch (category) {
        case Category::DEVICE:     return "WFB_Device";
        case Category::NETWORK:    return "WFB_Network";
        case Category::THREAD:     return "WFB_Thread";
        case Category::ADAPTIVE:   return "WFB_Adaptive";
        case Category::JNI:        return "WFB_JNI";
        case Category::CONFIG:     return "WFB_Config";
        case Category::AGGREGATOR: return "WFB_Aggregator";
        case Category::GENERAL:    return "WFB_General";
        default:                   return "WFB_Unknown";
    }
}

void WfbLogger::androidLog(Level level, const char* tag, const std::string& message) {
    int android_level;

    switch (level) {
        case Level::DEBUG:
            android_level = ANDROID_LOG_DEBUG;
            break;
        case Level::INFO:
            android_level = ANDROID_LOG_INFO;
            break;
        case Level::WARN:
            android_level = ANDROID_LOG_WARN;
            break;
        case Level::ERROR:
            android_level = ANDROID_LOG_ERROR;
            break;
        default:
            android_level = ANDROID_LOG_INFO;
            break;
    }

    __android_log_print(android_level, tag, "%s", message.c_str());
}

// LogContext implementation
LogContext::LogContext(WfbLogger::Category category, const std::string& function_name)
    : category_(category), function_name_(function_name),
      start_time_(std::chrono::steady_clock::now()) {

    WfbLogger::getInstance().debug(category_, "→ " + function_name_);
}

LogContext::~LogContext() {
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time_);

    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "← %s (%.3fms)",
                 function_name_.c_str(), duration.count() / 1000.0);

    WfbLogger::getInstance().debug(category_, std::string(buffer));
}

void LogContext::log(WfbLogger::Level level, const std::string& message) {
    std::string contextual_message = function_name_ + ": " + message;
    WfbLogger::getInstance().log(level, category_, contextual_message);
}