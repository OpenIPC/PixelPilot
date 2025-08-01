#include "WfbngLink.hpp"
#include "DeviceManager.hpp"
#include "AggregatorManager.hpp"
#include "ThreadManager.hpp"
#include "AdaptiveLinkController.hpp"
#include "PacketProcessor.hpp"

#include <android/log.h>
#include <jni.h>
#include <random>

#include "TxFrame.h"
#include "libusb.h"

#undef TAG
#define TAG "pixelpilot"

#define CRASH()                                                                                                        \
    do {                                                                                                               \
        int *i = 0;                                                                                                    \
        *i = 42;                                                                                                       \
    } while (0)

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

WfbngLink::WfbngLink(JNIEnv *env, jobject context, const WfbConfiguration& config)
        : config_(config), current_fd(-1), adaptive_link_enabled(config.adaptive_link.enabled_by_default),
          adaptive_tx_power(config.adaptive_link.default_tx_power) {
    Logger_t log;
    wifi_driver = std::make_shared<WiFiDriver>(log);

    // Initialize component managers
    device_manager = std::make_unique<DeviceManager>(wifi_driver);
    aggregator_manager = std::make_unique<AggregatorManager>(config_);
    thread_manager_ = std::make_unique<ThreadManager>();
    adaptive_controller_ = std::make_unique<AdaptiveLinkController>(config_, std::shared_ptr<DeviceManager>(device_manager.get(), [](DeviceManager*){}), fec);
    packet_processor_ = std::make_unique<PacketProcessor>(std::shared_ptr<AggregatorManager>(aggregator_manager.get(), [](AggregatorManager*){}), config_);

    // Initialize aggregators
    initAgg();
}

void WfbngLink::initAgg() {
    if (aggregator_manager) {
        aggregator_manager->initializeAggregators();
    }
}

int WfbngLink::run(JNIEnv *env, jobject context, jint wifiChannel, jint bw, jint fd) {
    int r;
    libusb_context *ctx = NULL;
    txFrame = std::make_shared<TxFrame>();

    r = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    r = libusb_init(&ctx);
    if (r < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to init libusb.");
        return r;
    }

    // Open adapters
    struct libusb_device_handle *dev_handle;
    r = libusb_wrap_sys_device(ctx, (intptr_t)fd, &dev_handle);
    if (r < 0) {
        libusb_exit(ctx);
        return r;
    }

    if (libusb_kernel_driver_active(dev_handle, 0)) {
        r = libusb_detach_kernel_driver(dev_handle, 0);
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "libusb_detach_kernel_driver: %d", r);
    }
    r = libusb_claim_interface(dev_handle, 0);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Creating driver and device for fd=%d", fd);

    auto device = device_manager->createDevice(fd, dev_handle);
    if (!device) {
        libusb_exit(ctx);
        __android_log_print(ANDROID_LOG_ERROR, TAG, "CreateRtlDevice error");
        return -1;
    }

    uint8_t *video_channel_id_be8 = reinterpret_cast<uint8_t *>(&video_channel_id_be);
    uint8_t *udp_channel_id_be8 = reinterpret_cast<uint8_t *>(&udp_channel_id_be);
    uint8_t *mavlink_channel_id_be8 = reinterpret_cast<uint8_t *>(&mavlink_channel_id_be);

    try {
        auto packetProcessor = [this](const Packet &packet) {
            if (aggregator_manager) {
                aggregator_manager->processPacket(packet);
            }
            if (should_clear_stats && aggregator_manager) {
                aggregator_manager->clearStats();
                should_clear_stats = false;
            }
        };

        // Store the current fd for later TX power updates.
        current_fd = fd;

        if (!usb_event_thread) {
            auto usb_event_thread_func = [ctx, this, fd] {
                while (true) {
                    auto device = this->device_manager->getDevice(fd);
                    if (!device || !device->isValid()) break;
                    auto timeout_us = config_.adaptive_link.usb_event_timeout.count();
                    struct timeval timeout = {0, static_cast<suseconds_t>(timeout_us)};
                    int r = libusb_handle_events_timeout(ctx, &timeout);
                    if (r < 0) {
                        this->log->error("Error handling events: {}", r);
                        // break;
                    }
                }
            };

            init_thread(usb_event_thread, [=]() { return std::make_unique<std::thread>(usb_event_thread_func); });

            std::shared_ptr<TxArgs> args = std::make_shared<TxArgs>();
            args->udp_port = config_.network_ports.UDP_TX_PORT;
            args->link_id = config_.device.link_id;
            args->keypair = config_.device.key_path.c_str();
            args->stbc = config_.phy.stbc_enabled;
            args->ldpc = config_.phy.ldpc_enabled;
            args->mcs_index = config_.phy.default_mcs_index;
            args->vht_mode = config_.phy.vht_mode;
            args->short_gi = config_.phy.short_gi;
            args->bandwidth = config_.phy.default_bandwidth;
            args->k = config_.fec.default_k;
            args->n = config_.fec.default_n;
            args->radio_port = config_.network_ports.WFB_TX_PORT;

            __android_log_print(
                ANDROID_LOG_ERROR, TAG, "radio link ID %d, radio PORT %d", args->link_id, args->radio_port);

            auto current_device_wrapper = device_manager->getDevice(fd);
            if (current_device_wrapper && !usb_tx_thread) {
                Rtl8812aDevice *current_device = current_device_wrapper->get();
                init_thread(usb_tx_thread, [&]() {
                    return std::make_unique<std::thread>([this, current_device, args] {
                        txFrame->run(current_device, args.get());
                        __android_log_print(ANDROID_LOG_DEBUG, TAG, "usb_transfer thread should terminate");
                    });
                });
            }

            __android_log_print(ANDROID_LOG_DEBUG, TAG, "Checking adaptive link: enabled=%d, controller=%p",
                                adaptive_link_enabled, adaptive_controller_.get());

            if (adaptive_link_enabled && adaptive_controller_) {
                bool started = adaptive_controller_->start(fd);
                __android_log_print(ANDROID_LOG_DEBUG, TAG, "Adaptive link start result: %d", started);
            } else {
                __android_log_print(ANDROID_LOG_WARN, TAG, "Adaptive link not started: enabled=%d, controller=%p",
                                   adaptive_link_enabled, adaptive_controller_.get());
            }
        }

        auto bandWidth = (bw == 20 ? CHANNEL_WIDTH_20 : CHANNEL_WIDTH_40);
        auto device = device_manager->getDevice(fd);
        if (device) {
            device->get()->Init(packetProcessor,
                               SelectedChannel{
                                   .Channel = static_cast<uint8_t>(wifiChannel),
                                   .ChannelOffset = 0,
                                   .ChannelWidth = bandWidth,
                               });
        }
    } catch (const std::runtime_error &error) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "runtime_error: %s", error.what());
        auto device = device_manager->getDevice(fd);
        if (device) {
            device->markForStop();
        }
        txFrame->stop();

        destroy_thread(usb_tx_thread);
        destroy_thread(usb_event_thread);
        stop_adaptive_link();
        return -1;
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Init done, releasing...");
    auto cleanup_device = device_manager->getDevice(fd);
    if (cleanup_device) {
        cleanup_device->markForStop();
    }
    txFrame->stop();

    destroy_thread(usb_tx_thread);
    destroy_thread(usb_event_thread);
    stop_adaptive_link();

    r = libusb_release_interface(dev_handle, 0);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "libusb_release_interface: %d", r);
    libusb_exit(ctx);
    return 0;
}

void WfbngLink::stop(JNIEnv *env, jobject context, jint fd) {
    if (!device_manager || !device_manager->hasDevice(fd)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Device with fd=%d not found", fd);
        CRASH();
        return;
    }
    auto device = device_manager->getDevice(fd);
    if (device) {
        device->markForStop();
    } else {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Device at fd=%d is nullptr", fd);
    }
    stop_adaptive_link();
}

void WfbngLink::stopDevice() {
    if (!device_manager || current_fd == -1) return;
    auto device = device_manager->getDevice(current_fd);
    if (device) {
        device->markForStop();
    }
}

void WfbngLink::setFecThresholds(int lostTo5, int recTo4, int recTo3, int recTo2, int recTo1) {
    config_.fec.lost_to_5 = lostTo5;
    config_.fec.recovered_to_4 = recTo4;
    config_.fec.recovered_to_3 = recTo3;
    config_.fec.recovered_to_2 = recTo2;
    config_.fec.recovered_to_1 = recTo1;
}

void WfbngLink::setAdaptiveLinkEnabled(bool enabled) {
    adaptive_link_enabled = enabled;

    if (adaptive_controller_) {
        adaptive_controller_->setEnabled(enabled);

        if (enabled && current_fd != -1) {
            adaptive_controller_->start(current_fd);
        } else if (!enabled) {
            adaptive_controller_->stop();
        }
    }
}

void WfbngLink::setAdaptiveTxPower(int power) {
    adaptive_tx_power = power;

    if (adaptive_controller_) {
        adaptive_controller_->setTxPower(power);
    }
}

void WfbngLink::stop_adaptive_link() {
    if (adaptive_controller_) {
        adaptive_controller_->stop();
    }
}

// JNI functions have been moved to WfbngLinkJNI.cpp for better separation of concerns