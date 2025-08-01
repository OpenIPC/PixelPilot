#include "AdaptiveLinkController.hpp"
#include "DeviceManager.hpp"
#include "WfbLogger.hpp"

#include <android/log.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#undef TAG
#define TAG "AdaptiveLinkController"

AdaptiveLinkController::AdaptiveLinkController(const WfbConfiguration& config,
                                             std::shared_ptr<DeviceManager> device_manager,
                                             FecChangeController& fec_controller)
    : config_(config)
    , device_manager_(device_manager)
    , fec_controller_(fec_controller)
    , enabled_(config.adaptive_link.enabled_by_default)
    , tx_power_(config.adaptive_link.default_tx_power) {

    WFB_LOGF_INFO(ADAPTIVE, "AdaptiveLinkController initialized: enabled=%d, tx_power=%d",
                  enabled_.load(), tx_power_.load());
}

AdaptiveLinkController::~AdaptiveLinkController() {
    stop();
}

bool AdaptiveLinkController::start(int device_fd) {
    WFB_LOG_CONTEXT(ADAPTIVE, "AdaptiveLinkController::start");

    if (running_.load()) {
        WFB_LOG_WARN(ADAPTIVE, "Controller already running");
        return false;
    }

    if (!enabled_.load()) {
        WFB_LOG_DEBUG(ADAPTIVE, "Controller disabled, not starting");
        return false;
    }

    current_device_fd_ = device_fd;
    should_stop_ = false;
    running_ = true;

    // Set initial TX power on device
    auto device = device_manager_->getDevice(device_fd);
    if (device) {
        device->setTxPower(tx_power_.load());
    }

    // Start monitoring thread
    monitoring_thread_ = std::make_unique<std::thread>(&AdaptiveLinkController::linkQualityLoop, this);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Started adaptive link monitoring for fd=%d", device_fd);
    return true;
}

void AdaptiveLinkController::stop() {
    if (!running_.load()) {
        return;
    }

    should_stop_ = true;
    running_ = false;

    if (monitoring_thread_ && monitoring_thread_->joinable()) {
        monitoring_thread_->join();
        monitoring_thread_.reset();
    }

    current_device_fd_ = -1;
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stopped adaptive link monitoring");
}

void AdaptiveLinkController::setEnabled(bool enabled) {
    bool was_enabled = enabled_.exchange(enabled);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "setEnabled(%d): was_enabled=%d, current_fd=%d",
                       enabled, was_enabled, current_device_fd_.load());

    if (enabled && !was_enabled && current_device_fd_ != -1) {
        // Enabling - restart if we have a device
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Enabling adaptive link, restarting...");
        stop();
        start(current_device_fd_);
    } else if (!enabled && was_enabled) {
        // Disabling - stop monitoring
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Disabling adaptive link, stopping...");
        stop();
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Adaptive link %s", enabled ? "enabled" : "disabled");
}

void AdaptiveLinkController::setTxPower(int power) {
    int old_power = tx_power_.exchange(power);

    if (old_power == power) {
        return; // No change
    }

    // Update TX power on all devices
    if (device_manager_) {
        device_manager_->forEachDevice([power](int device_fd, std::shared_ptr<DeviceManager::Device> device) {
            if (device && device->isValid()) {
                device->setTxPower(power);
                WFB_LOGF_DEBUG(ADAPTIVE, "Updated TX power to %d for device fd=%d", power, device_fd);
            }
        });
    }

    // If adaptive mode is enabled and running, restart to apply new power
    if (enabled_.load() && running_.load()) {
        int current_fd = current_device_fd_.load();
        stop();
        if (current_fd != -1) {
            start(current_fd);
        }
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "TX power updated to %d", power);
}

void AdaptiveLinkController::linkQualityLoop() {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Link quality monitoring thread started");

    // Wait a bit before starting monitoring
    std::this_thread::sleep_for(std::chrono::seconds(1));

    struct sockaddr_in server_addr;
    int sockfd = createSocket(server_addr);
    if (sockfd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to create UDP socket");
        running_ = false;
        return;
    }

    while (!should_stop_.load()) {
        try {
            auto quality = SignalQualityCalculator::get_instance().calculate_signal_quality();

            #if defined(ANDROID_DEBUG_RSSI) || true
            __android_log_print(ANDROID_LOG_WARN, TAG, "Signal quality: %d", quality.quality);
            #endif

            // Map quality to configured range
            quality.quality = static_cast<int>(mapRange(quality.quality,
                                                      config_.adaptive_link.signal_quality_min,
                                                      config_.adaptive_link.signal_quality_max,
                                                      config_.adaptive_link.mapped_quality_min,
                                                      config_.adaptive_link.mapped_quality_max));

            // Update FEC settings based on quality
            updateFecSettings(quality);

            // Send quality update to ground station
            sendQualityUpdate(quality, sockfd, server_addr);

        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in link quality loop: %s", e.what());
        }

        // Sleep for configured interval
        std::this_thread::sleep_for(config_.adaptive_link.update_interval);
    }

    close(sockfd);
    running_ = false;
    should_stop_ = false;

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Link quality monitoring thread stopped");
}

void AdaptiveLinkController::sendQualityUpdate(const SignalQualityCalculator::SignalQuality& quality,
                                              int sockfd,
                                              const struct sockaddr_in& server_addr) {
    uint32_t len;
    char message[100];
    time_t currentEpoch = time(nullptr);

    /**
     * Message format:
     * <gs_time>:<link_score>:<link_score>:<fec>:<lost>:<rssi_dB>:<snr_dB>:<num_ants>:<noise_penalty>:<fec_change>:<idr_request_code>
     *
     * gs_time: ground station clock
     * link_score: 1000-2000 sent twice (already including any penalty)
     * fec: instantaneous fec_rec (only used by old fec_rec_pntly now disabled by default)
     * lost: instantaneous lost (not used)
     * rssi_dB: best antenna rssi (for OSD)
     * snr_dB: best antenna snr_dB (for OSD)
     * num_ants: number of gs antennas (for OSD)
     * noise_penalty: penalty deducted from score due to noise (for OSD)
     * fec_change: int from 0 to 5 : how much to alter fec based on noise
     * optional idr_request_code: 4 char unique code to request 1 keyframe
     */

    snprintf(message + sizeof(len),
             sizeof(message) - sizeof(len),
             "%ld:%d:%d:%d:%d:%d:%f:0:-1:%d:%s\n",
             static_cast<long>(currentEpoch),
             quality.quality,
             quality.quality,
             quality.recovered_last_second,
             quality.lost_last_second,
             quality.quality,
             quality.snr,
             fec_controller_.value(),
             quality.idr_code.c_str());

    len = strlen(message + sizeof(len));
    len = htonl(len);
    memcpy(message, &len, sizeof(len));

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Sending message: %s", message + 4);

    ssize_t sent = sendto(sockfd,
                         message,
                         strlen(message + sizeof(len)) + sizeof(len),
                         0,
                         (struct sockaddr *)&server_addr,
                         sizeof(server_addr));

    if (sent < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to send quality message: %s", strerror(errno));
    }
}

int AdaptiveLinkController::createSocket(struct sockaddr_in& server_addr) {
    const char* ip = config_.adaptive_link.target_ip.c_str();
    int port = config_.network_ports.ADAPTIVE_LINK_PORT;

    // Create UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Socket creation failed: %s", strerror(errno));
        return -1;
    }

    // Set socket options
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid IP address: %s", ip);
        close(sockfd);
        return -1;
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Created UDP socket for %s:%d", ip, port);
    return sockfd;
}

void AdaptiveLinkController::updateFecSettings(const SignalQualityCalculator::SignalQuality& quality) {
    // Use the configuration FEC threshold values to adjust FEC
    if (quality.lost_last_second > config_.fec.lost_to_5) {
        fec_controller_.bump(5); // Bump to FEC 5
    } else if (quality.recovered_last_second > config_.fec.recovered_to_4) {
        fec_controller_.bump(4); // Bump to FEC 4
    } else if (quality.recovered_last_second > config_.fec.recovered_to_3) {
        fec_controller_.bump(3); // Bump to FEC 3
    } else if (quality.recovered_last_second > config_.fec.recovered_to_2) {
        fec_controller_.bump(2); // Bump to FEC 2
    } else if (quality.recovered_last_second > config_.fec.recovered_to_1) {
        fec_controller_.bump(1); // Bump to FEC 1
    }
}

double AdaptiveLinkController::mapRange(double value, double inputMin, double inputMax,
                                       double outputMin, double outputMax) {
    return outputMin + ((value - inputMin) * (outputMax - outputMin) / (inputMax - inputMin));
}