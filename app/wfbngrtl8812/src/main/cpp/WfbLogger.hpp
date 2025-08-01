#ifndef WFB_LOGGER_HPP
#define WFB_LOGGER_HPP

#include <string>
#include <sstream>
#include <memory>
#include <chrono>
#include <cstdio>

/**
 * @brief Structured logging system for WFB components
 *
 * This class provides a centralized logging system with categories and levels,
 * making it easier to filter and analyze logs. It wraps Android's logging
 * system while providing additional structure and type safety.
 */
class WfbLogger {
public:
    /**
     * @brief Logging levels in order of severity
     */
    enum class Level {
        DEBUG = 0,    ///< Detailed debug information
        INFO = 1,     ///< General information
        WARN = 2,     ///< Warning messages
        ERROR = 3     ///< Error messages
    };

    /**
     * @brief Logging categories for different system components
     */
    enum class Category {
        DEVICE,        ///< Device management and hardware operations
        NETWORK,       ///< Network operations and packet processing
        THREAD,        ///< Thread management and lifecycle
        ADAPTIVE,      ///< Adaptive link quality and control
        JNI,           ///< JNI interface and Java bindings
        CONFIG,        ///< Configuration management
        AGGREGATOR,    ///< Packet aggregation and processing
        GENERAL        ///< General purpose logging
    };

    /**
     * @brief Get the singleton logger instance
     * @return Reference to the global logger instance
     */
    static WfbLogger& getInstance();

    /**
     * @brief Log a message with specified level and category
     * @param level Logging level
     * @param category Component category
     * @param message Log message
     */
    void log(Level level, Category category, const std::string& message);

    /**
     * @brief Log a formatted message with specified level and category
     * @tparam Args Variadic template arguments
     * @param level Logging level
     * @param category Component category
     * @param format Format string
     * @param args Format arguments
     */
    template<typename... Args>
    void logf(Level level, Category category, const char* format, Args... args) {
        if (!shouldLog(level)) {
            return;
        }

        // Format the message
        char buffer[1024];
        int result = std::snprintf(buffer, sizeof(buffer), format, args...);

        if (result > 0 && result < static_cast<int>(sizeof(buffer))) {
            log(level, category, std::string(buffer));
        } else {
            // Fallback for oversized messages
            log(Level::ERROR, Category::GENERAL, "Log message too long or formatting error");
        }
    }

    /**
     * @brief Log a debug message
     * @param category Component category
     * @param message Log message
     */
    void debug(Category category, const std::string& message);

    /**
     * @brief Log an info message
     * @param category Component category
     * @param message Log message
     */
    void info(Category category, const std::string& message);

    /**
     * @brief Log a warning message
     * @param category Component category
     * @param message Log message
     */
    void warn(Category category, const std::string& message);

    /**
     * @brief Log an error message
     * @param category Component category
     * @param message Log message
     */
    void error(Category category, const std::string& message);

    /**
     * @brief Set the minimum logging level
     * @param level Minimum level to log
     */
    void setMinLevel(Level level);

    /**
     * @brief Check if a level should be logged
     * @param level Level to check
     * @return true if the level should be logged
     */
    bool shouldLog(Level level) const;

    /**
     * @brief Convert logging level to string
     * @param level Logging level
     * @return String representation of the level
     */
    static const char* levelToString(Level level);

    /**
     * @brief Convert category to string
     * @param category Logging category
     * @return String representation of the category
     */
    static const char* categoryToString(Category category);

    /**
     * @brief Convert category to Android log tag
     * @param category Logging category
     * @return Android log tag for the category
     */
    static const char* categoryToTag(Category category);

private:
    /**
     * @brief Private constructor for singleton pattern
     */
    WfbLogger() = default;

    /**
     * @brief Send log message to Android logging system
     * @param level Logging level
     * @param tag Android log tag
     * @param message Log message
     */
    void androidLog(Level level, const char* tag, const std::string& message);

    Level min_level_{Level::DEBUG};  ///< Minimum logging level
};

/**
 * @brief RAII-style log context for scoped logging
 *
 * This class provides automatic logging of function entry/exit with timing,
 * useful for debugging performance and control flow.
 */
class LogContext {
public:
    /**
     * @brief Constructor that logs function entry
     * @param category Logging category
     * @param function_name Name of the function
     */
    LogContext(WfbLogger::Category category, const std::string& function_name);

    /**
     * @brief Destructor that logs function exit with timing
     */
    ~LogContext();

    /**
     * @brief Log a message within this context
     * @param level Logging level
     * @param message Log message
     */
    void log(WfbLogger::Level level, const std::string& message);

private:
    WfbLogger::Category category_;
    std::string function_name_;
    std::chrono::steady_clock::time_point start_time_;
};

// Convenience macros for common logging operations
#define WFB_LOG_DEBUG(category, message) \
    WfbLogger::getInstance().debug(WfbLogger::Category::category, message)

#define WFB_LOG_INFO(category, message) \
    WfbLogger::getInstance().info(WfbLogger::Category::category, message)

#define WFB_LOG_WARN(category, message) \
    WfbLogger::getInstance().warn(WfbLogger::Category::category, message)

#define WFB_LOG_ERROR(category, message) \
    WfbLogger::getInstance().error(WfbLogger::Category::category, message)

#define WFB_LOG_CONTEXT(category, function) \
    LogContext _log_ctx(WfbLogger::Category::category, function)

// Formatted logging macros
#define WFB_LOGF_DEBUG(category, format, ...) \
    WfbLogger::getInstance().logf(WfbLogger::Level::DEBUG, WfbLogger::Category::category, format, __VA_ARGS__)

#define WFB_LOGF_INFO(category, format, ...) \
    WfbLogger::getInstance().logf(WfbLogger::Level::INFO, WfbLogger::Category::category, format, __VA_ARGS__)

#define WFB_LOGF_WARN(category, format, ...) \
    WfbLogger::getInstance().logf(WfbLogger::Level::WARN, WfbLogger::Category::category, format, __VA_ARGS__)

#define WFB_LOGF_ERROR(category, format, ...) \
    WfbLogger::getInstance().logf(WfbLogger::Level::ERROR, WfbLogger::Category::category, format, __VA_ARGS__)

#endif // WFB_LOGGER_HPP