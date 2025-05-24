#pragma once

#include <android/log.h>
#include <chrono>
#include <mutex>

class FecChangeController {
    static constexpr const char *TAG = "FecChangeController";

  public:
    /// \brief Query the current (possibly decayed) fec_change value.
    ///        Call this as often as you like; the class handles its own timing.
    int value() {
        if (!mEnabled) return 0;

        std::lock_guard<std::mutex> lock(mx_);
        decayLocked_();
        return val_;
    }

    /// \brief Raise fec_change.  If newValue <= current, the call is ignored.
    ///        A successful bump resets the 5-second “hold” timer.
    void bump(int newValue) {
        std::lock_guard<std::mutex> lock(mx_);
        if (newValue > val_) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "bumping FEC: %d", newValue);

            val_ = newValue;
            lastChange_ = Clock::now();
        }
    }

    void setEnabled(bool use) { mEnabled = use; }

  private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kTick{1}; // length of one hold/decay window

    void decayLocked_() {
        if (val_ == 0) return;

        auto now = Clock::now();
        auto elapsed = now - lastChange_;

        // Still inside the mandatory 5-second hold?  Do nothing.
        if (elapsed < kTick) return;

        // How many *full* ticks have passed since lastChange_?
        auto ticks = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() / kTick.count();
        if (ticks == 0) return; // safety net (shouldn’t hit)

        int decayed = val_ - static_cast<int>(ticks);
        if (decayed < 0) decayed = 0;

        // Commit the decay and anchor lastChange_ on the most recent tick boundary
        if (decayed != val_) {
            val_ = decayed;
            lastChange_ += kTick * ticks;
        }
    }

    int val_{0};
    Clock::time_point lastChange_{Clock::now()};
    std::mutex mx_;
    bool mEnabled = false;
};
