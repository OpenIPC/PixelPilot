#ifndef PACKET_PROCESSOR_HPP
#define PACKET_PROCESSOR_HPP

#include "WfbConfiguration.hpp"
#include "RxFrame.h"
#include "devourer/src/FrameParser.h"

#include <memory>
#include <functional>
#include <atomic>

// Forward declarations
class AggregatorManager;

/**
 * @brief Handles packet processing and routing
 *
 * This class extracts the packet processing functionality from WfbngLink to provide
 * a dedicated component for managing:
 * - Packet validation
 * - Packet routing to appropriate aggregators
 * - Statistics management coordination
 * - Channel ID management
 */
class PacketProcessor {
public:
    /**
     * @brief Constructs the packet processor
     * @param aggregator_manager Reference to aggregator manager for packet routing
     * @param config Configuration containing channel settings
     */
    explicit PacketProcessor(std::shared_ptr<AggregatorManager> aggregator_manager,
                           const WfbConfiguration& config);

    /**
     * @brief Destructor
     */
    ~PacketProcessor() = default;

    /**
     * @brief Processes an incoming packet
     * @param packet The packet to process
     * @return true if packet was processed successfully, false otherwise
     */
    bool processPacket(const Packet& packet);

    /**
     * @brief Requests stats to be cleared on next packet processing
     */
    void requestStatsClear() { should_clear_stats_ = true; }

    /**
     * @brief Gets packet processing statistics
     * @return Structure containing processing stats
     */
    struct ProcessingStats {
        uint64_t total_packets_processed = 0;
        uint64_t valid_packets = 0;
        uint64_t invalid_packets = 0;
        uint64_t video_packets = 0;
        uint64_t mavlink_packets = 0;
        uint64_t udp_packets = 0;
        uint64_t unknown_packets = 0;
    };

    ProcessingStats getStats() const { return stats_; }

    /**
     * @brief Resets processing statistics
     */
    void resetStats();

private:
    /**
     * @brief Validates that a packet is a valid WFB frame
     * @param packet Packet to validate
     * @return true if valid, false otherwise
     */
    bool isValidWfbPacket(const Packet& packet);

    /**
     * @brief Determines packet type based on channel ID
     * @param frame Parsed frame data
     * @return Packet type enum
     */
    enum class PacketType {
        VIDEO,
        MAVLINK,
        UDP,
        UNKNOWN
    };

    PacketType determinePacketType(const RxFrame& frame);

    /**
     * @brief Routes packet to appropriate aggregator
     * @param packet Packet to route
     * @param type Packet type determined from channel ID
     */
    void routePacket(const Packet& packet, PacketType type);

    // Dependencies
    std::shared_ptr<AggregatorManager> aggregator_manager_;
    WfbConfiguration config_;

    // Channel IDs for packet routing
    uint32_t video_channel_id_be_;
    uint32_t mavlink_channel_id_be_;
    uint32_t udp_channel_id_be_;

    // Statistics and state
    mutable std::atomic<bool> should_clear_stats_{false};
    mutable ProcessingStats stats_;
};

#endif // PACKET_PROCESSOR_HPP