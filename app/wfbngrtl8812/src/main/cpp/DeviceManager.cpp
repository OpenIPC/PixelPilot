#include "DeviceManager.hpp"
#include <android/log.h>
#include <algorithm>

#define TAG "DeviceManager"

// DeviceManager::Device implementation
DeviceManager::Device::Device(std::unique_ptr<Rtl8812aDevice> rtl_device, int fd)
    : rtl_device_(std::move(rtl_device)), fd_(fd), valid_(true) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Device created for fd=%d", fd);
}

DeviceManager::Device::~Device() {
    if (rtl_device_) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Device destroyed for fd=%d", fd_);
        // Mark for stopping if not already done
        markForStop();
    }
}

Rtl8812aDevice* DeviceManager::Device::get() const {
    return rtl_device_.get();
}

bool DeviceManager::Device::isValid() const {
    return valid_ && rtl_device_ && !rtl_device_->should_stop;
}

void DeviceManager::Device::markForStop() {
    valid_ = false;
    if (rtl_device_) {
        rtl_device_->should_stop = true;
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Device marked for stop, fd=%d", fd_);
    }
}

int DeviceManager::Device::getFileDescriptor() const {
    return fd_;
}

void DeviceManager::Device::setTxPower(int power) {
    if (rtl_device_ && valid_) {
        rtl_device_->SetTxPower(power);
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "TX power set to %d for fd=%d", power, fd_);
    }
}

// DeviceManager implementation
DeviceManager::DeviceManager(std::shared_ptr<WiFiDriver> wifi_driver)
    : wifi_driver_(std::move(wifi_driver)) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "DeviceManager created");
}

DeviceManager::~DeviceManager() {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "DeviceManager destructor - cleaning up %zu devices", devices_.size());
    destroyAllDevices();
}

std::shared_ptr<DeviceManager::Device> DeviceManager::createDevice(int fd, struct libusb_device_handle* dev_handle) {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    // Check if device already exists
    if (devices_.find(fd) != devices_.end()) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Device with fd=%d already exists", fd);
        return devices_[fd];
    }

    return createDeviceInternal(fd, dev_handle);
}

std::shared_ptr<DeviceManager::Device> DeviceManager::createDeviceInternal(int fd, struct libusb_device_handle* dev_handle) {
    if (!wifi_driver_) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "WiFi driver not available");
        return nullptr;
    }

    try {
        // Create RTL device using WiFi driver
        std::unique_ptr<Rtl8812aDevice> rtl_device = wifi_driver_->CreateRtlDevice(dev_handle);
        if (!rtl_device) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to create RTL device for fd=%d", fd);
            return nullptr;
        }

        // Create our RAII wrapper
        auto device = std::make_shared<Device>(std::move(rtl_device), fd);
        devices_[fd] = device;

        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Successfully created device for fd=%d", fd);
        return device;

    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Exception creating device for fd=%d: %s", fd, e.what());
        return nullptr;
    }
}

void DeviceManager::destroyDevice(int fd) {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    auto it = devices_.find(fd);
    if (it != devices_.end()) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Destroying device for fd=%d", fd);

        // Mark device for stop before removing
        it->second->markForStop();
        devices_.erase(it);

        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Device destroyed for fd=%d", fd);
    } else {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Attempted to destroy non-existent device fd=%d", fd);
    }
}

std::shared_ptr<DeviceManager::Device> DeviceManager::getDevice(int fd) const {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    auto it = devices_.find(fd);
    if (it != devices_.end()) {
        return it->second;
    }

    return nullptr;
}

bool DeviceManager::hasDevice(int fd) const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return devices_.find(fd) != devices_.end();
}

size_t DeviceManager::getDeviceCount() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return devices_.size();
}

void DeviceManager::stopAllDevices() {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Stopping all %zu devices", devices_.size());

    for (auto& [fd, device] : devices_) {
        if (device) {
            device->markForStop();
        }
    }
}

void DeviceManager::destroyAllDevices() {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Destroying all %zu devices", devices_.size());

    // Mark all devices for stop first
    for (auto& [fd, device] : devices_) {
        if (device) {
            device->markForStop();
        }
    }

    // Clear the map (devices will be destroyed when their shared_ptr count reaches 0)
    devices_.clear();
}

void DeviceManager::forEachDevice(const std::function<void(int, std::shared_ptr<Device>)>& func) const {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    for (const auto& [fd, device] : devices_) {
        if (device && func) {
            func(fd, device);
        }
    }
}

