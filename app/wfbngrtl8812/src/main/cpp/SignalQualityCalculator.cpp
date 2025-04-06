#include "SignalQualityCalculator.h"
#include <android/log.h>
#include <chrono>
#include <random>

namespace {

std::string generate_random_string(size_t length) {
    const std::string characters = "abcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, characters.size() - 1);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += characters[distrib(gen)];
    }
    return result;
}

} // namespace

// Remove RSSI samples older than 1 second
void SignalQualityCalculator::cleanup_old_rssi_data() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - kAveragingWindow;

    // Erase-remove idiom for data older than cutoff
    m_rssis.erase(std::remove_if(
                      m_rssis.begin(), m_rssis.end(), [&](const RssiEntry &entry) { return entry.timestamp < cutoff; }),
                  m_rssis.end());
}

void SignalQualityCalculator::cleanup_old_snr_data() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - kAveragingWindow;

    // Erase-remove idiom for data older than cutoff
    m_snrs.erase(
        std::remove_if(m_snrs.begin(), m_snrs.end(), [&](const SnrEntry &entry) { return entry.timestamp < cutoff; }),
        m_snrs.end());
}

void SignalQualityCalculator::cleanup_old_fec_data() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - kAveragingWindow;

    m_fec_data.erase(std::remove_if(m_fec_data.begin(),
                                    m_fec_data.end(),
                                    [&](const FecEntry &entry) { return entry.timestamp < cutoff; }),
                     m_fec_data.end());
}

// Add a new RSSI entry with current timestamp
void SignalQualityCalculator::add_rssi(uint8_t ant1, uint8_t ant2) {
    //__android_log_print(ANDROID_LOG_WARN, TAG, "rssi1 %d, rssi2 %d", (int)ant1, (int)ant2);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    RssiEntry entry;
    entry.timestamp = std::chrono::steady_clock::now();
    entry.ant1 = ant1;
    entry.ant2 = ant2;
    m_rssis.push_back(entry);
}

void SignalQualityCalculator::add_snr(int8_t ant1, int8_t ant2) {
    //__android_log_print(ANDROID_LOG_WARN, TAG, "rssi1 %d, rssi2 %d", (int)ant1, (int)ant2);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    SnrEntry entry;
    entry.timestamp = std::chrono::steady_clock::now();
    entry.ant1 = ant1;
    entry.ant2 = ant2;
    m_snrs.push_back(entry);
}

// Calculate signal quality based on last-second RSSI and FEC data
SignalQualityCalculator::SignalQuality SignalQualityCalculator::calculate_signal_quality() {
    SignalQuality ret;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Get fresh averages over the last second
    float avg_rssi = get_avg(m_rssis);

    float avg_snr = get_avg(m_snrs);

    //    __android_log_print(ANDROID_LOG_DEBUG, TAG, "avg_rssi: %f", avg_rssi);

    // Map the RSSI from range 10..80 to -1024..1024
    avg_rssi = map_range(avg_rssi, 0.f, 80.f, -1024.f, 1024.f);
    avg_rssi = std::max(-1024.f, std::min(1024.f, avg_rssi));

    // Return final clamped quality
    // formula: quality = avg_rssi - p_recovered * 5 - p_lost * 100
    // clamp between -1024 and 1024
    auto [p_recovered, p_lost] = get_accumulated_fec_data();

    //    __android_log_print(ANDROID_LOG_ERROR, "DEBUG", "FEC RECOVERED %d, FEC_LOST %d", p_recovered, p_lost);

    float quality = avg_rssi - static_cast<float>(p_recovered) * 12.f - static_cast<float>(p_lost) * 40.f;
    quality = std::max(-1024.f, std::min(1024.f, quality));

    ret.quality = quality;
    ret.lost_last_second = p_lost;
    ret.recovered_last_second = p_recovered;

    ret.snr = avg_snr;
    ret.idr_code = m_idr_code;

    cleanup_old_rssi_data();
    cleanup_old_snr_data();
    cleanup_old_fec_data();

    /* __android_log_print(ANDROID_LOG_DEBUG,
                         TAG,
                         "QUALITY: %f, RSSI: %f, P_RECOVERED: %u, P_LOST: %u",
                         quality,
                         avg_rssi,
                         p_recovered,
                         p_lost);
 */
    return ret;
}

// Sum up FEC data over the last 1 second
std::pair<uint32_t, uint32_t> SignalQualityCalculator::get_accumulated_fec_data() {
    // Make sure we clean up old FEC data first
    cleanup_old_fec_data();

    //    __android_log_print(ANDROID_LOG_ERROR, "LOST size", "%zu", m_fec_data.size());

    uint32_t p_recovered = 0;
    uint32_t p_all = 0;
    uint32_t p_lost = 0;
    for (const auto &data : m_fec_data) {
        p_all += data.all;
        p_recovered += data.recovered;
        p_lost += data.lost;
    }
    if (p_all == 0) return {300, 300};
    return {p_recovered, p_lost};
}

// Add new FEC data entry with current timestamp
void SignalQualityCalculator::add_fec_data(uint32_t p_all, uint32_t p_recovered, uint32_t p_lost) {
    //    __android_log_print(ANDROID_LOG_ERROR, "RECOVERED + LOST", "%u + %u", p_recovered, p_lost);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    FecEntry entry;
    entry.timestamp = std::chrono::steady_clock::now();
    entry.all = p_all;
    entry.recovered = p_recovered;
    entry.lost = p_lost;

    if (p_lost > 0) {
        m_idr_code = generate_random_string(4);
    }

    m_fec_data.push_back(entry);
}
