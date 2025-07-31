#pragma once

#include <memory>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <functional>
#include <atomic>
#include <vector>

/**
 * @brief Manages thread lifecycle with named thread tracking
 *
 * Provides RAII-based thread management with proper cleanup and
 * thread safety for concurrent access.
 */
class ThreadManager {
public:
    /**
     * @brief Constructor
     */
    ThreadManager();

    /**
     * @brief Destructor - ensures all threads are properly joined
     */
    ~ThreadManager();

    /**
     * @brief Start a new named thread
     * @tparam Func Function type to execute in the thread
     * @param name Unique name for the thread
     * @param func Function to execute in the thread
     * @return true if thread was started successfully, false if name already exists
     */
    template<typename Func>
    bool startThread(const std::string& name, Func&& func);

    /**
     * @brief Stop and join a specific thread
     * @param name Name of the thread to stop
     * @return true if thread was found and stopped, false otherwise
     */
    bool stopThread(const std::string& name);

    /**
     * @brief Stop and join all threads
     */
    void stopAllThreads();

    /**
     * @brief Check if a thread with given name exists
     * @param name Thread name to check
     * @return true if thread exists and is joinable
     */
    bool hasThread(const std::string& name) const;

    /**
     * @brief Get count of active threads
     * @return Number of active threads
     */
    size_t getThreadCount() const;

    /**
     * @brief Get list of all thread names
     * @return Vector of thread names
     */
    std::vector<std::string> getThreadNames() const;

    /**
     * @brief Check if thread is still running
     * @param name Thread name to check
     * @return true if thread exists and is still running
     */
    bool isThreadRunning(const std::string& name) const;

    /**
     * @brief Request thread to stop (thread-safe signal)
     * @param name Thread name
     * @return true if stop signal was sent, false if thread doesn't exist
     */
    bool requestThreadStop(const std::string& name);

    /**
     * @brief Check if stop was requested for a thread
     * @param name Thread name
     * @return true if stop was requested
     */
    bool isStopRequested(const std::string& name) const;

private:
    /**
     * @brief Internal thread info structure
     */
    struct ThreadInfo {
        std::unique_ptr<std::thread> thread;
        std::atomic<bool> stop_requested{false};

        ThreadInfo() = default;
        ThreadInfo(std::unique_ptr<std::thread> t) : thread(std::move(t)), stop_requested(false) {}

        // Non-copyable, movable (custom move constructor/assignment due to atomic)
        ThreadInfo(const ThreadInfo&) = delete;
        ThreadInfo& operator=(const ThreadInfo&) = delete;

        ThreadInfo(ThreadInfo&& other) noexcept
            : thread(std::move(other.thread)), stop_requested(other.stop_requested.load()) {}

        ThreadInfo& operator=(ThreadInfo&& other) noexcept {
            if (this != &other) {
                thread = std::move(other.thread);
                stop_requested.store(other.stop_requested.load());
            }
            return *this;
        }
    };

    /**
     * @brief Internal thread stop and cleanup (caller must hold mutex)
     * @param name Thread name
     * @return true if thread was found and stopped
     */
    bool stopThreadInternal(const std::string& name);

    /**
     * @brief Cleanup finished threads (caller must hold mutex)
     */
    void cleanupFinishedThreads();

    mutable std::recursive_mutex threads_mutex_;
    std::map<std::string, ThreadInfo> threads_;
};

// Template implementation
template<typename Func>
bool ThreadManager::startThread(const std::string& name, Func&& func) {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);

    // Check if thread with this name already exists
    if (threads_.find(name) != threads_.end()) {
        return false;
    }

    try {
        // Create the thread with a wrapper that allows for stop checking
        auto thread_ptr = std::make_unique<std::thread>([this, name, func = std::forward<Func>(func)]() {
            try {
                func();
            } catch (const std::exception& e) {
                // Log exception but don't crash
                // Note: Could add logging here if logger is available
            }
        });

        ThreadInfo info(std::move(thread_ptr));
        threads_[name] = std::move(info);

        return true;

    } catch (const std::exception& e) {
        // Failed to create thread
        return false;
    }
}