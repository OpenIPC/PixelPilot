#pragma once

#include "devourer/src/WiFiDriver.h"
#include "WfbConfiguration.hpp"
#include "WfbLogger.hpp"
#include <memory>
#include <map>
#include <mutex>
#include <functional>

/**
 * @brief RAII wrapper for RTL8812 device management
 *
 * Manages the lifecycle of RTL devices with proper cleanup and thread safety.
 */
class DeviceManager {
public:
    /**
     * @brief RAII wrapper for individual RTL device
     */
    class Device {
    public:
        Device(std::unique_ptr<Rtl8812aDevice> rtl_device, int fd);
        ~Device();

        // Non-copyable, movable
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
        Device(Device&&) = default;
        Device& operator=(Device&&) = default;

        /**
         * @brief Get the underlying RTL device
         * @return Raw pointer to RTL device (managed by this wrapper)
         */
        Rtl8812aDevice* get() const;

        /**
         * @brief Check if device is valid
         * @return true if device is valid and not marked for stopping
         */
        bool isValid() const;

        /**
         * @brief Mark device for stopping
         */
        void markForStop();

        /**
         * @brief Get the file descriptor associated with this device
         * @return File descriptor
         */
        int getFileDescriptor() const;

        /**
         * @brief Set TX power for this device
         * @param power TX power value
         */
        void setTxPower(int power);

    private:
        std::unique_ptr<Rtl8812aDevice> rtl_device_;
        int fd_;
        bool valid_;
    };

    /**
     * @brief Constructor
     * @param wifi_driver Shared pointer to WiFi driver
     */
    explicit DeviceManager(std::shared_ptr<WiFiDriver> wifi_driver);

    /**
     * @brief Destructor - ensures all devices are properly cleaned up
     */
    ~DeviceManager();

    /**
     * @brief Create a new device
     * @param fd File descriptor for the device
     * @param dev_handle libusb device handle
     * @return Shared pointer to the created device, or nullptr on failure
     */
    std::shared_ptr<Device> createDevice(int fd, struct libusb_device_handle* dev_handle);

    /**
     * @brief Remove and cleanup a device
     * @param fd File descriptor of the device to remove
     */
    void destroyDevice(int fd);

    /**
     * @brief Get an existing device
     * @param fd File descriptor of the device
     * @return Shared pointer to the device, or nullptr if not found
     */
    std::shared_ptr<Device> getDevice(int fd) const;

    /**
     * @brief Check if a device exists
     * @param fd File descriptor to check
     * @return true if device exists
     */
    bool hasDevice(int fd) const;

    /**
     * @brief Get count of managed devices
     * @return Number of devices currently managed
     */
    size_t getDeviceCount() const;

    /**
     * @brief Mark all devices for stopping
     */
    void stopAllDevices();

    /**
     * @brief Remove all devices
     */
    void destroyAllDevices();

    /**
     * @brief Apply function to all devices
     * @param func Function to apply (receives fd and device)
     */
    void forEachDevice(const std::function<void(int, std::shared_ptr<Device>)>& func) const;

private:
    mutable std::mutex devices_mutex_;
    std::map<int, std::shared_ptr<Device>> devices_;
    std::shared_ptr<WiFiDriver> wifi_driver_;

    /**
     * @brief Internal device creation without mutex (caller must hold lock)
     */
    std::shared_ptr<Device> createDeviceInternal(int fd, struct libusb_device_handle* dev_handle);
};