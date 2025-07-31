#ifndef ADAPTIVE_LINK_CONTROLLER_HPP
#define ADAPTIVE_LINK_CONTROLLER_HPP

#include "WfbConfiguration.hpp"
#include "FecChangeController.h"
#include "SignalQualityCalculator.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <functional>

// Forward declarations
class DeviceManager;

/**
 * @brief Manages adaptive link quality monitoring and control
 *
 * This class extracts the adaptive link functionality from WfbngLink to provide
 * a dedicated component for managing:
 * - Signal quality monitoring
 * - FEC adaptation based on link quality
 * - TX power control
 * - UDP communication with ground station
 */
class AdaptiveLinkController {
public:
    /**
     * @brief Constructs the controller with configuration
     * @param config Configuration containing adaptive link settings
     * @param device_manager Reference to device manager for power control
     * @param fec_controller Reference to FEC controller for adaptation
     */
    explicit AdaptiveLinkController(const WfbConfiguration& config,
                                   std::shared_ptr<DeviceManager> device_manager,
                                   FecChangeController& fec_controller);

    /**
     * @brief Destructor - ensures clean shutdown
     */
    ~AdaptiveLinkController();

    /**
     * @brief Starts the adaptive link quality monitoring
     * @param device_fd File descriptor of the device to monitor
     * @return true if started successfully, false otherwise
     */
    bool start(int device_fd);

    /**
     * @brief Stops the adaptive link monitoring
     */
    void stop();

    /**
     * @brief Enables or disables adaptive link functionality
     * @param enabled true to enable, false to disable
     */
    void setEnabled(bool enabled);

    /**
     * @brief Sets the transmission power
     * @param power TX power value
     */
    void setTxPower(int power);

    /**
     * @brief Checks if adaptive link is currently enabled
     * @return true if enabled, false otherwise
     */
    bool isEnabled() const { return enabled_.load(); }

    /**
     * @brief Checks if adaptive link is currently running
     * @return true if running, false otherwise
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Gets the current TX power setting
     * @return Current TX power value
     */
    int getTxPower() const { return tx_power_.load(); }

private:
    /**
     * @brief Main loop for link quality monitoring
     * Runs in a separate thread to continuously monitor and adapt link quality
     */
    void linkQualityLoop();

    /**
     * @brief Sends quality update message to ground station
     * @param quality Signal quality data to send
     * @param sockfd UDP socket file descriptor
     * @param server_addr Server address structure
     */
    void sendQualityUpdate(const SignalQualityCalculator::SignalQuality& quality, int sockfd,
                          const struct sockaddr_in& server_addr);

    /**
     * @brief Creates and configures UDP socket for communication
     * @param server_addr Output parameter for server address
     * @return Socket file descriptor, or -1 on error
     */
    int createSocket(struct sockaddr_in& server_addr);

    /**
     * @brief Updates FEC settings based on signal quality
     * @param quality Current signal quality measurements
     */
    void updateFecSettings(const SignalQualityCalculator::SignalQuality& quality);

    /**
     * @brief Maps signal quality to configured range
     * @param value Input value to map
     * @param inputMin Input range minimum
     * @param inputMax Input range maximum
     * @param outputMin Output range minimum
     * @param outputMax Output range maximum
     * @return Mapped value
     */
    double mapRange(double value, double inputMin, double inputMax,
                   double outputMin, double outputMax);

    // Configuration
    WfbConfiguration config_;
    std::shared_ptr<DeviceManager> device_manager_;
    FecChangeController& fec_controller_;

    // Thread control
    std::atomic<bool> enabled_{true};
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
    std::unique_ptr<std::thread> monitoring_thread_;

    // Settings
    std::atomic<int> tx_power_{30};
    std::atomic<int> current_device_fd_{-1};
};

#endif // ADAPTIVE_LINK_CONTROLLER_HPP