#pragma once

#include "WfbConfiguration.hpp"
#include "RxFrame.h"
#include "SignalQualityCalculator.h"
#include <memory>
#include <mutex>
#include <cstdint>

// Forward declarations
struct Packet;
class AggregatorUDPv4;

/**
 * @brief Manages network aggregation for video, mavlink, and UDP streams
 *
 * Encapsulates the aggregator initialization and packet processing logic
 * with proper thread safety and configuration management.
 */
class AggregatorManager {
public:
    /**
     * @brief Constructor
     * @param config Configuration containing network settings
     */
    explicit AggregatorManager(const WfbConfiguration& config);

    /**
     * @brief Destructor
     */
    ~AggregatorManager();

    /**
     * @brief Initialize all aggregators
     *
     * Creates video, mavlink, and UDP aggregators with the configured settings.
     * This should be called after construction and whenever configuration changes.
     */
    void initializeAggregators();

    /**
     * @brief Process an incoming packet
     * @param packet The packet to process
     *
     * Routes the packet to the appropriate aggregator based on channel ID.
     */
    void processPacket(const Packet& packet);

    /**
     * @brief Clear statistics for all aggregators
     */
    void clearStats();

    /**
     * @brief Get video aggregator statistics
     * @return Pointer to video aggregator (for stats access), or nullptr if not initialized
     */
    AggregatorUDPv4* getVideoAggregator() const;

    /**
     * @brief Get mavlink aggregator statistics
     * @return Pointer to mavlink aggregator (for stats access), or nullptr if not initialized
     */
    AggregatorUDPv4* getMavlinkAggregator() const;

    /**
     * @brief Get UDP aggregator statistics
     * @return Pointer to UDP aggregator (for stats access), or nullptr if not initialized
     */
    AggregatorUDPv4* getUdpAggregator() const;

    /**
     * @brief Check if aggregators are initialized
     * @return true if all aggregators are created and ready
     */
    bool isInitialized() const;

    /**
     * @brief Update configuration and reinitialize aggregators
     * @param new_config New configuration to apply
     */
    void updateConfiguration(const WfbConfiguration& new_config);

    /**
     * @brief Get current configuration
     * @return Reference to current configuration
     */
    const WfbConfiguration& getConfiguration() const;

private:
    /**
     * @brief Process video packet
     * @param packet The packet to process
     * @param frame Parsed frame information
     */
    void processVideoPacket(const Packet& packet, const RxFrame& frame);

    /**
     * @brief Process mavlink packet
     * @param packet The packet to process
     * @param frame Parsed frame information
     */
    void processMavlinkPacket(const Packet& packet, const RxFrame& frame);

    /**
     * @brief Process UDP packet
     * @param packet The packet to process
     * @param frame Parsed frame information
     */
    void processUdpPacket(const Packet& packet, const RxFrame& frame);

    /**
     * @brief Calculate channel ID in big-endian format
     * @param radio_port Radio port for the channel
     * @return Channel ID in big-endian format
     */
    uint32_t calculateChannelId(uint8_t radio_port) const;

    /**
     * @brief Setup common packet processing parameters
     * @param packet Source packet
     * @param antenna Output antenna array
     * @param rssi Output RSSI array
     * @param noise Output noise array
     * @param freq Output frequency
     */
    void setupPacketParams(const Packet& packet, uint8_t antenna[4], int8_t rssi[4],
                          int8_t noise[4], uint32_t& freq) const;

    // Configuration
    WfbConfiguration config_;

    // Aggregators
    std::unique_ptr<AggregatorUDPv4> video_aggregator_;
    std::unique_ptr<AggregatorUDPv4> mavlink_aggregator_;
    std::unique_ptr<AggregatorUDPv4> udp_aggregator_;

    // Channel IDs (in big-endian format)
    uint32_t video_channel_id_be_;
    uint32_t mavlink_channel_id_be_;
    uint32_t udp_channel_id_be_;

    // Thread safety
    mutable std::mutex aggregator_mutex_;

    // State
    bool initialized_;
    bool should_clear_stats_;
};