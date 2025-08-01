#include "PacketProcessor.hpp"
#include "AggregatorManager.hpp"

#include <android/log.h>
#include <arpa/inet.h>

#undef TAG
#define TAG "PacketProcessor"

PacketProcessor::PacketProcessor(std::shared_ptr<AggregatorManager> aggregator_manager,
                               const WfbConfiguration& config)
    : aggregator_manager_(aggregator_manager)
    , config_(config) {

    // Initialize channel IDs from configuration
    video_channel_id_be_ = htonl(config_.network_ports.VIDEO_CLIENT_PORT);
    mavlink_channel_id_be_ = htonl(config_.network_ports.MAVLINK_CLIENT_PORT);
    udp_channel_id_be_ = htonl(config_.network_ports.UDP_CLIENT_PORT);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "PacketProcessor initialized with channel IDs: video=%d, mavlink=%d, udp=%d",
                       config_.network_ports.VIDEO_CLIENT_PORT,
                       config_.network_ports.MAVLINK_CLIENT_PORT,
                       config_.network_ports.UDP_CLIENT_PORT);
}

bool PacketProcessor::processPacket(const Packet& packet) {
    stats_.total_packets_processed++;

    if (!aggregator_manager_) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Cannot process packet - aggregator manager not available");
        stats_.invalid_packets++;
        return false;
    }

    // Validate packet
    if (!isValidWfbPacket(packet)) {
        stats_.invalid_packets++;
        return false;
    }

    stats_.valid_packets++;

    // Create RxFrame for channel ID detection
    RxFrame frame(packet.Data);
    if (!frame.IsValidWfbFrame()) {
        stats_.invalid_packets++;
        return false;
    }

    // Determine packet type and route appropriately
    PacketType type = determinePacketType(frame);
    routePacket(packet, type);

    // Update type-specific statistics
    switch (type) {
        case PacketType::VIDEO:
            stats_.video_packets++;
            break;
        case PacketType::MAVLINK:
            stats_.mavlink_packets++;
            break;
        case PacketType::UDP:
            stats_.udp_packets++;
            break;
        case PacketType::UNKNOWN:
            stats_.unknown_packets++;
            break;
    }

    // Handle stats clearing if requested
    if (should_clear_stats_.exchange(false)) {
        aggregator_manager_->clearStats();
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Cleared aggregator stats");
    }

    return true;
}

void PacketProcessor::resetStats() {
    stats_ = ProcessingStats{};
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Reset packet processing statistics");
}

bool PacketProcessor::isValidWfbPacket(const Packet& packet) {
    // Basic validation - check if packet has valid data span
    if (packet.Data.empty()) {
        return false;
    }

    // Additional validation could be added here
    return true;
}

PacketProcessor::PacketType PacketProcessor::determinePacketType(const RxFrame& frame) {
    uint8_t* video_channel_id_be8 = reinterpret_cast<uint8_t*>(&video_channel_id_be_);
    uint8_t* mavlink_channel_id_be8 = reinterpret_cast<uint8_t*>(&mavlink_channel_id_be_);
    uint8_t* udp_channel_id_be8 = reinterpret_cast<uint8_t*>(&udp_channel_id_be_);

    if (frame.MatchesChannelID(video_channel_id_be8)) {
        return PacketType::VIDEO;
    } else if (frame.MatchesChannelID(mavlink_channel_id_be8)) {
        return PacketType::MAVLINK;
    } else if (frame.MatchesChannelID(udp_channel_id_be8)) {
        return PacketType::UDP;
    } else {
        return PacketType::UNKNOWN;
    }
}

void PacketProcessor::routePacket(const Packet& packet, PacketType type) {
    try {
        // Route to aggregator manager for actual processing
        aggregator_manager_->processPacket(packet);

        #ifdef DEBUG_PACKET_ROUTING
        const char* type_name = "unknown";
        switch (type) {
            case PacketType::VIDEO: type_name = "video"; break;
            case PacketType::MAVLINK: type_name = "mavlink"; break;
            case PacketType::UDP: type_name = "udp"; break;
            case PacketType::UNKNOWN: type_name = "unknown"; break;
        }
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Routed %s packet (size=%zu)", type_name, packet.Data.size());
        #endif

    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error routing packet: %s", e.what());
        stats_.invalid_packets++;
    }
}