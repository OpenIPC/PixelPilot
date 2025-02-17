#pragma once
#include <algorithm>
#include <android/log.h>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

// Adjust as needed
static const char *TAG = "SignalQualityCalculator";

class SignalQualityCalculator {
  public:
    SignalQualityCalculator() = default;
    ~SignalQualityCalculator() = default;

    float get_avg_rssi();
    void add_rssi(uint8_t ant1, uint8_t ant2);
    struct SignalQuality {
        int lost_last_second;
        int recovered_last_second;
        int quality;
    };
    SignalQuality calculate_signal_quality();
    void add_fec_data(uint32_t p_all, uint32_t p_recovered, uint32_t p_lost);
    static SignalQualityCalculator &get_instance() {
        static SignalQualityCalculator instance;
        return instance;
    }

  private:
    std::pair<uint32_t, uint32_t> get_accumulated_fec_data();

    // Helper methods to remove old entries
    void cleanup_old_rssi_data();
    void cleanup_old_fec_data();

    // We store a timestamp for each RSSI entry
    struct RssiEntry {
        std::chrono::steady_clock::time_point timestamp;
        uint8_t ant1;
        uint8_t ant2;
    };

    // We store a timestamp for each FEC entry
    struct FecEntry {
        std::chrono::steady_clock::time_point timestamp;
        uint32_t all;
        uint32_t recovered;
        uint32_t lost;
    };

    double map_range(double value, double inputMin, double inputMax, double outputMin, double outputMax) {
        return outputMin + ((value - inputMin) * (outputMax - outputMin) / (inputMax - inputMin));
    }

  private:
    mutable std::recursive_mutex m_mutex;

    // Changed from std::vector<std::pair<uint8_t, uint8_t>> to store timestamps
    std::vector<RssiEntry> m_rssis;

    // Changed from std::vector<std::pair<uint32_t, uint32_t>> to store timestamps
    std::vector<FecEntry> m_fec_data;
};
