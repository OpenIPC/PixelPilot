#include "AggregatorManager.hpp"
#include "wfb-ng/src/fec.h"
#include "wfb-ng/src/rx.hpp"
#include "devourer/src/FrameParser.h"
#include <android/log.h>
#include <arpa/inet.h>
#include <cstring>

#define TAG "AggregatorManager"

AggregatorManager::AggregatorManager(const WfbConfiguration& config)
    : config_(config), initialized_(false), should_clear_stats_(false) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "AggregatorManager created");
}

AggregatorManager::~AggregatorManager() {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "AggregatorManager destroyed");

    // Aggregators will be automatically destroyed by smart ptr
    video_aggregator_.reset();
    mavlink_aggregator_.reset();
    udp_aggregator_.reset();
}

void AggregatorManager::initializeAggregators() {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Initializing aggregators");

    const std::string& client_addr = config_.adaptive_link.client_ip;
    uint64_t epoch = config_.device.epoch;
    const char* key_path = config_.device.key_path.c_str();

    try {
        // Initialize video aggregator
        uint8_t video_radio_port = 0;
        uint32_t video_channel_id_f = (config_.device.link_id << 8) + video_radio_port;
        video_channel_id_be_ = htobe32(video_channel_id_f);

        video_aggregator_ = std::make_unique<AggregatorUDPv4>(
            client_addr,
            config_.network_ports.VIDEO_CLIENT_PORT,
            key_path,
            epoch,
            video_channel_id_f,
            0
        );

        // Initialize mavlink aggregator
        uint8_t mavlink_radio_port = config_.network_ports.MAVLINK_RADIO_PORT;
        uint32_t mavlink_channel_id_f = (config_.device.link_id << 8) + mavlink_radio_port;
        mavlink_channel_id_be_ = htobe32(mavlink_channel_id_f);

        mavlink_aggregator_ = std::make_unique<AggregatorUDPv4>(
            client_addr,
            config_.network_ports.MAVLINK_CLIENT_PORT,
            key_path,
            epoch,
            mavlink_channel_id_f,
            0
        );

        // Initialize UDP aggregator
        uint8_t udp_radio_port = config_.network_ports.WFB_RX_PORT;
        uint32_t udp_channel_id_f = (config_.device.link_id << 8) + udp_radio_port;
        udp_channel_id_be_ = htobe32(udp_channel_id_f);

        __android_log_print(ANDROID_LOG_WARN, TAG, "UDP Channel ID: link_id=%d, radio_port=%d, channel_id_f=0x%x, channel_id_be=0x%x",
                           config_.device.link_id, udp_radio_port, udp_channel_id_f, udp_channel_id_be_);

        udp_aggregator_ = std::make_unique<AggregatorUDPv4>(
            client_addr,
            config_.network_ports.UDP_CLIENT_PORT,
            key_path,
            epoch,
            udp_channel_id_f,
            0
        );

        initialized_ = true;
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Aggregators initialized successfully");

    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to initialize aggregators: %s", e.what());

        // Clean up partially initialized aggregators
        video_aggregator_.reset();
        mavlink_aggregator_.reset();
        udp_aggregator_.reset();
        initialized_ = false;
    }
}

void AggregatorManager::processPacket(const Packet& packet) {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);

    if (!initialized_) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Cannot process packet - aggregators not initialized");
        return;
    }

    RxFrame frame(packet.Data);
    if (!frame.IsValidWfbFrame()) {
        return;
    }

    // Setup common packet parameters
    uint8_t antenna[4];
    int8_t rssi[4];
    int8_t noise[4];
    uint32_t freq;
    setupPacketParams(packet, antenna, rssi, noise, freq);

    // Route packet based on channel ID
    uint8_t* video_channel_id_be8 = reinterpret_cast<uint8_t*>(&video_channel_id_be_);
    uint8_t* mavlink_channel_id_be8 = reinterpret_cast<uint8_t*>(&mavlink_channel_id_be_);
    uint8_t* udp_channel_id_be8 = reinterpret_cast<uint8_t*>(&udp_channel_id_be_);

    if (frame.MatchesChannelID(video_channel_id_be8)) {
        processVideoPacket(packet, frame);
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Processed VIDEO packet");
    } else if (frame.MatchesChannelID(mavlink_channel_id_be8)) {
        processMavlinkPacket(packet, frame);
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Processed MAVLINK packet");
    } else if (frame.MatchesChannelID(udp_channel_id_be8)) {
        processUdpPacket(packet, frame);
        __android_log_print(ANDROID_LOG_WARN, TAG, "Processed UDP packet (VPN)");
    } else {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Unknown packet type - not matching any channel ID");
    }

    // Handle stats clearing if requested
    if (should_clear_stats_) {
        clearStats();
        should_clear_stats_ = false;
    }
}

void AggregatorManager::processVideoPacket(const Packet& packet, const RxFrame& frame) {
    if (!video_aggregator_) return;

    // Update signal quality metrics for video packets
    SignalQualityCalculator::get_instance().add_rssi(packet.RxAtrib.rssi[0], packet.RxAtrib.rssi[1]);
    SignalQualityCalculator::get_instance().add_snr(packet.RxAtrib.snr[0], packet.RxAtrib.snr[1]);

    // Setup packet parameters
    uint8_t antenna[4] = {1, 1, 1, 1};
    int8_t rssi[4] = {(int8_t)packet.RxAtrib.rssi[0], (int8_t)packet.RxAtrib.rssi[1], 1, 1};
    int8_t noise[4] = {1, 1, 1, 1};
    uint32_t freq = 0;

    // Process packet through video aggregator
    video_aggregator_->process_packet(
        packet.Data.data() + sizeof(ieee80211_header),
        packet.Data.size() - sizeof(ieee80211_header) - 4,
        0,
        antenna,
        rssi,
        noise,
        freq,
        0,
        0,
        NULL
    );
}

void AggregatorManager::processMavlinkPacket(const Packet& packet, const RxFrame& frame) {
    if (!mavlink_aggregator_) return;

    // Setup packet parameters
    uint8_t antenna[4] = {1, 1, 1, 1};
    int8_t rssi[4] = {(int8_t)packet.RxAtrib.rssi[0], (int8_t)packet.RxAtrib.rssi[1], 1, 1};
    int8_t noise[4] = {1, 1, 1, 1};
    uint32_t freq = 0;

    // Process packet through mavlink aggregator
    mavlink_aggregator_->process_packet(
        packet.Data.data() + sizeof(ieee80211_header),
        packet.Data.size() - sizeof(ieee80211_header) - 4,
        0,
        antenna,
        rssi,
        noise,
        freq,
        0,
        0,
        NULL
    );
}

void AggregatorManager::processUdpPacket(const Packet& packet, const RxFrame& frame) {
    if (!udp_aggregator_) return;

    // Setup packet parameters
    uint8_t antenna[4] = {1, 1, 1, 1};
    int8_t rssi[4] = {(int8_t)packet.RxAtrib.rssi[0], (int8_t)packet.RxAtrib.rssi[1], 1, 1};
    int8_t noise[4] = {1, 1, 1, 1};
    uint32_t freq = 0;

    // Process packet through UDP aggregator
    udp_aggregator_->process_packet(
        packet.Data.data() + sizeof(ieee80211_header),
        packet.Data.size() - sizeof(ieee80211_header) - 4,
        0,
        antenna,
        rssi,
        noise,
        freq,
        0,
        0,
        NULL
    );
}

void AggregatorManager::clearStats() {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);

    if (video_aggregator_) {
        video_aggregator_->clear_stats();
    }
    if (mavlink_aggregator_) {
        mavlink_aggregator_->clear_stats();
    }
    if (udp_aggregator_) {
        udp_aggregator_->clear_stats();
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Aggregator stats cleared");
}

AggregatorUDPv4* AggregatorManager::getVideoAggregator() const {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);
    return video_aggregator_.get();
}

AggregatorUDPv4* AggregatorManager::getMavlinkAggregator() const {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);
    return mavlink_aggregator_.get();
}

AggregatorUDPv4* AggregatorManager::getUdpAggregator() const {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);
    return udp_aggregator_.get();
}

bool AggregatorManager::isInitialized() const {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);
    return initialized_ && video_aggregator_ && mavlink_aggregator_ && udp_aggregator_;
}

void AggregatorManager::updateConfiguration(const WfbConfiguration& new_config) {
    std::lock_guard<std::mutex> lock(aggregator_mutex_);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Updating configuration");
    config_ = new_config;

    // Reinitialize aggregators with new configuration
    initialized_ = false;
    video_aggregator_.reset();
    mavlink_aggregator_.reset();
    udp_aggregator_.reset();

    // Initialize with new config (unlock temporarily to avoid recursive lock)
    lock.~lock_guard();
    initializeAggregators();
}

const WfbConfiguration& AggregatorManager::getConfiguration() const {
    return config_;
}

uint32_t AggregatorManager::calculateChannelId(uint8_t radio_port) const {
    uint32_t channel_id_f = (config_.device.link_id << 8) + radio_port;
    return htobe32(channel_id_f);
}

void AggregatorManager::setupPacketParams(const Packet& packet, uint8_t antenna[4],
                                         int8_t rssi[4], int8_t noise[4], uint32_t& freq) const {
    // Setup antenna array
    antenna[0] = antenna[1] = antenna[2] = antenna[3] = 1;

    // Setup RSSI array
    rssi[0] = static_cast<int8_t>(packet.RxAtrib.rssi[0]);
    rssi[1] = static_cast<int8_t>(packet.RxAtrib.rssi[1]);
    rssi[2] = rssi[3] = 1;

    // Setup noise array
    noise[0] = noise[1] = noise[2] = noise[3] = 1;

    // Setup frequency
    freq = 0;
}