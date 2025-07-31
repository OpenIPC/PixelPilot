#pragma once

#include <chrono>
#include <string>
#include <cstdint>

/**
 * @brief Central configuration management for WfbngLink
 *
 * This class consolidates all hardcoded values from WfbngLink into
 * structured configuration objects for better maintainability.
 */
class WfbConfiguration {
public:
    /**
     * @brief Network port configurations
     */
    struct NetworkPorts {
        static constexpr int VIDEO_CLIENT_PORT = 5600;
        static constexpr int MAVLINK_CLIENT_PORT = 14550;
        static constexpr int UDP_CLIENT_PORT = 8000;
        static constexpr int ADAPTIVE_LINK_PORT = 9999;
        static constexpr int UDP_TX_PORT = 8001;
        static constexpr uint8_t WFB_TX_PORT = 160;
        static constexpr uint8_t WFB_RX_PORT = 32;
        static constexpr uint8_t MAVLINK_RADIO_PORT = 0x10;
    };

    /**
     * @brief Adaptive link quality management configuration
     */
    struct AdaptiveLinkConfig {
        std::string target_ip = "10.5.0.10";
        std::string client_ip = "127.0.0.1";
        std::chrono::milliseconds update_interval{100};
        std::chrono::microseconds usb_event_timeout{500000}; // 500ms
        int default_tx_power = 30;
        bool enabled_by_default = true;

        // Signal quality mapping parameters
        int signal_quality_min = -1024;
        int signal_quality_max = 1024;
        int mapped_quality_min = 1000;
        int mapped_quality_max = 2000;
    };

    /**
     * @brief Forward Error Correction (FEC) configuration
     */
    struct FecConfig {
        // FEC switching thresholds
        int lost_to_5 = 2;          // Lost packets threshold to switch to FEC 5
        int recovered_to_4 = 30;    // Recovered packets threshold to switch to FEC 4
        int recovered_to_3 = 24;    // Recovered packets threshold to switch to FEC 3
        int recovered_to_2 = 14;    // Recovered packets threshold to switch to FEC 2
        int recovered_to_1 = 8;     // Recovered packets threshold to switch to FEC 1

        // FEC transmission parameters
        int default_k = 1;          // Number of data packets
        int default_n = 5;          // Total packets (data + redundancy)

        bool enabled_by_default = false;
    };

    /**
     * @brief Physical layer configuration
     */
    struct PhyConfig {
        bool ldpc_enabled = true;   // Low-Density Parity-Check
        bool stbc_enabled = true;   // Space-Time Block Coding
        bool vht_mode = false;      // Very High Throughput mode
        bool short_gi = false;      // Short Guard Interval
        int default_bandwidth = 20; // Channel bandwidth in MHz
        int default_mcs_index = 0;  // Modulation and Coding Scheme index
    };

    /**
     * @brief Device and channel configuration
     */
    struct DeviceConfig {
        uint32_t link_id = 7669206; // Default link identifier
        std::string key_path = "/data/user/0/com.openipc.pixelpilot/files/gs.key";
        uint64_t epoch = 0;         // Default epoch for aggregators

        // Channel width constants
        enum class ChannelWidth {
            WIDTH_20MHZ = 20,
            WIDTH_40MHZ = 40
        };
    };

    /**
     * @brief Thread and timing configuration
     */
    struct ThreadConfig {
        std::chrono::seconds fec_decay_tick{1};     // FEC controller decay interval
        std::chrono::milliseconds stats_callback_interval{300}; // Statistics callback frequency
        int max_concurrent_devices = 8;             // Maximum number of RTL devices
    };

    /**
     * @brief Logging configuration
     */
    struct LogConfig {
        std::string tag = "pixelpilot";
        bool debug_rssi_enabled = true;
        bool verbose_logging = false;
    };

    // Configuration instances
    NetworkPorts network_ports;
    AdaptiveLinkConfig adaptive_link;
    FecConfig fec;
    PhyConfig phy;
    DeviceConfig device;
    ThreadConfig threading;
    LogConfig logging;

    /**
     * @brief Create default configuration
     */
    static WfbConfiguration createDefault() {
        return WfbConfiguration{};
    }

    /**
     * @brief Create configuration optimized for testing
     */
    static WfbConfiguration createForTesting() {
        WfbConfiguration config;
        config.adaptive_link.update_interval = std::chrono::milliseconds{10};
        config.threading.stats_callback_interval = std::chrono::milliseconds{50};
        config.logging.verbose_logging = true;
        return config;
    }

    /**
     * @brief Validate configuration values
     * @return true if configuration is valid, false otherwise
     */
    bool validate() const {
        // Validate port ranges
        if (network_ports.VIDEO_CLIENT_PORT <= 0 || network_ports.VIDEO_CLIENT_PORT > 65535) {
            return false;
        }
        if (network_ports.MAVLINK_CLIENT_PORT <= 0 || network_ports.MAVLINK_CLIENT_PORT > 65535) {
            return false;
        }
        if (network_ports.UDP_CLIENT_PORT <= 0 || network_ports.UDP_CLIENT_PORT > 65535) {
            return false;
        }

        // Validate adaptive link config
        if (adaptive_link.default_tx_power < 0 || adaptive_link.default_tx_power > 100) {
            return false;
        }
        if (adaptive_link.update_interval.count() <= 0) {
            return false;
        }

        // Validate FEC thresholds
        if (fec.lost_to_5 < 0 || fec.recovered_to_1 < 0) {
            return false;
        }

        // Validate PHY config
        if (phy.default_bandwidth != 20 && phy.default_bandwidth != 40) {
            return false;
        }

        return true;
    }
};