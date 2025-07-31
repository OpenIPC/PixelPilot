#include "ThreadManager.hpp"
#include <android/log.h>
#include <algorithm>

#define TAG "ThreadManager"

ThreadManager::ThreadManager() {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "ThreadManager created");
}

ThreadManager::~ThreadManager() {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "ThreadManager destructor - stopping %zu threads", threads_.size());
    stopAllThreads();
}

bool ThreadManager::stopThread(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);
    return stopThreadInternal(name);
}

bool ThreadManager::stopThreadInternal(const std::string& name) {
    auto it = threads_.find(name);
    if (it == threads_.end()) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Cannot stop thread '%s' - not found", name.c_str());
        return false;
    }

    ThreadInfo& info = it->second;

    // Signal thread to stop
    info.stop_requested.store(true);

    // Join the thread if it's joinable
    if (info.thread && info.thread->joinable()) {
        try {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Joining thread '%s'", name.c_str());
            info.thread->join();
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Thread '%s' joined successfully", name.c_str());
        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Exception joining thread '%s': %s", name.c_str(), e.what());
        }
    }

    // Remove from map
    threads_.erase(it);
    return true;
}

void ThreadManager::stopAllThreads() {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stopping all %zu threads", threads_.size());

    // First, signal all threads to stop
    for (auto& [name, info] : threads_) {
        info.stop_requested.store(true);
    }

    // Then join all threads
    std::vector<std::string> thread_names;
    for (const auto& [name, info] : threads_) {
        thread_names.push_back(name);
    }

    for (const std::string& name : thread_names) {
        stopThreadInternal(name);
    }

    threads_.clear();
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "All threads stopped");
}

bool ThreadManager::hasThread(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);

    auto it = threads_.find(name);
    if (it == threads_.end()) {
        return false;
    }

    return it->second.thread && it->second.thread->joinable();
}

size_t ThreadManager::getThreadCount() const {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);
    return threads_.size();
}

std::vector<std::string> ThreadManager::getThreadNames() const {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);

    std::vector<std::string> names;
    names.reserve(threads_.size());

    for (const auto& [name, info] : threads_) {
        names.push_back(name);
    }

    return names;
}

bool ThreadManager::isThreadRunning(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);

    auto it = threads_.find(name);
    if (it == threads_.end()) {
        return false;
    }

    const ThreadInfo& info = it->second;
    return info.thread && info.thread->joinable() && !info.stop_requested.load();
}

bool ThreadManager::requestThreadStop(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);

    auto it = threads_.find(name);
    if (it == threads_.end()) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Cannot request stop for thread '%s' - not found", name.c_str());
        return false;
    }

    it->second.stop_requested.store(true);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stop requested for thread '%s'", name.c_str());
    return true;
}

bool ThreadManager::isStopRequested(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(threads_mutex_);

    auto it = threads_.find(name);
    if (it == threads_.end()) {
        return false;
    }

    return it->second.stop_requested.load();
}

void ThreadManager::cleanupFinishedThreads() {
    // This method is called with mutex already held
    std::vector<std::string> finished_threads;

    for (const auto& [name, info] : threads_) {
        if (!info.thread || !info.thread->joinable()) {
            finished_threads.push_back(name);
        }
    }

    for (const std::string& name : finished_threads) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Cleaning up finished thread '%s'", name.c_str());
        threads_.erase(name);
    }
}