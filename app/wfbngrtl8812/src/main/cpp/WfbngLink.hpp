#ifndef FPV_VR_WFBNG_LINK_H
#define FPV_VR_WFBNG_LINK_H

#include "FecChangeController.h"
#include "SignalQualityCalculator.h"
#include "StaLink.h"
#include "TxFrame.h"

extern "C" {
#include "wfb-ng/src/zfex.h"
}

#include "devourer/src/IRtlDevice.h"
#include "devourer/src/WiFiDriver.h"
#include "wfb-ng/src/rx.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <jni.h>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector> // Added for std::vector

struct libusb_context;

const uint8_t wfb_tx_port = 160;
const uint8_t wfb_rx_port = 32;

// How the adapter is used. Must match the constants in WfbNgLink.java.
enum class LinkMode : int {
    // wfb-ng broadcast in monitor mode (the original behaviour).
    WFB = 0,
    // Regular Wi-Fi station joining the camera's open access point (StaLink).
    AP = 1,
};

class WfbngLink {
  public:
    // Takes effect on the next run(); the Java side restarts the adapters after changing it.
    std::atomic<LinkMode> link_mode{LinkMode::WFB};
    std::mutex sta_mutex;
    StaConfig sta_config;            // guarded by sta_mutex
    // Guarded by sta_mutex; set while an AP-mode run() is active. Shared so a camera
    // command can keep using it without holding sta_mutex for seconds; run() stops it
    // before the device goes away, after which every call on it is a no-op.
    std::shared_ptr<StaLink> sta_link;

    // FEC switching thresholds (for menu)
    int fec_lost_to_5 = 2;
    int fec_recovered_to_4 = 30;
    int fec_recovered_to_3 = 24;
    int fec_recovered_to_2 = 14;
    int fec_recovered_to_1 = 8;
    WfbngLink(JNIEnv *env, jobject context);

    int run(JNIEnv *env, jobject androidContext, jint wifiChannel, jint bw, jint fd);

    void initAgg();

    void stop(JNIEnv *env, jobject androidContext, jint fd);

    std::mutex agg_mutex;
    std::unique_ptr<AggregatorUDPv4> video_aggregator;
    std::unique_ptr<AggregatorUDPv4> mavlink_aggregator;
    std::unique_ptr<AggregatorUDPv4> udp_aggregator;

    void start_link_quality_thread(int fd);

    // adaptive link
    // TODO: move this to private section
    int current_fd;
    bool adaptive_link_enabled;
    bool adaptive_link_should_stop{false};
    int adaptive_tx_power;

    // Runtime configurable PHY parameters
    bool ldpc_enabled{true};
    bool stbc_enabled{true};

    std::map<int, std::shared_ptr<IRtlDevice>> rtl_devices;

    // Set by stop() and read by run(). A StopRxLoop() only takes effect once the RX loop is
    // running: RtlJaguarDevice::StartRxLoop() clears should_stop on entry, so a stop that
    // lands anywhere before that - including the whole chip bring-up in InitWrite(), which
    // is the longest part of run() - is thrown away, and run() then blocks in a loop nobody
    // asked for. Recorded here instead, so run() can see it at the points where the flag
    // itself cannot be trusted.
    std::mutex stop_requested_mutex;
    std::set<int> stop_requested_fds;

    void note_stop_requested(int fd) {
        std::lock_guard<std::mutex> lock(stop_requested_mutex);
        stop_requested_fds.insert(fd);
    }

    // Cleared at the start of run(): fd numbers are reused, so a request left over from a
    // previous session on the same number must not abort the new one.
    void clear_stop_request(int fd) {
        std::lock_guard<std::mutex> lock(stop_requested_mutex);
        stop_requested_fds.erase(fd);
    }

    bool stop_requested(int fd) {
        std::lock_guard<std::mutex> lock(stop_requested_mutex);
        return stop_requested_fds.count(fd) > 0;
    }
    std::unique_ptr<std::thread> link_quality_thread{nullptr};
    bool should_clear_stats{false};
    FecChangeController fec;

    void init_thread(std::unique_ptr<std::thread> &thread,
                     const std::function<std::unique_ptr<std::thread>()> &init_func) {
        std::unique_lock<std::recursive_mutex> lock(thread_mutex);
        destroy_thread(thread);
        thread = init_func();
    }

    void destroy_thread(std::unique_ptr<std::thread> &thread) {
        std::unique_lock<std::recursive_mutex> lock(thread_mutex);
        if (thread && thread->joinable()) {
            thread->join();
            thread = nullptr;
        }
    }

    void stop_adaptive_link() {
        std::unique_lock<std::recursive_mutex> lock(thread_mutex);

        if (!link_quality_thread) return;
        this->adaptive_link_should_stop = true;
        destroy_thread(link_quality_thread);
    }

  private:
    // AP-mode half of run(), entered once the device exists. Owns the device
    // shutdown and libusb teardown, like the rest of run() does for wfb-ng mode.
    int runStation(libusb_device_handle *dev_handle, libusb_context *ctx, int fd, int wifiChannel);

    // Destroys the device object once run() is done with it. devourer's per-adapter
    // lock lives inside that object, so as long as it stays in rtl_devices the next
    // run() - after a mode, channel or settings change - is refused with "USB adapter
    // in use ... by another devourer process". Must come before libusb_exit().
    void releaseDevice(int fd) {
        rtl_devices.erase(fd);
        if (current_fd == fd) current_fd = -1;
    }

    void stopDevice() {
        if (rtl_devices.find(current_fd) == rtl_devices.end()) return;
        auto dev = rtl_devices.at(current_fd).get();
        if (dev) {
            dev->StopRxLoop();
        }
    }

    const char *keyPath = "/data/user/0/com.openipc.pixelpilot/files/gs.key";
    std::recursive_mutex thread_mutex;
    std::unique_ptr<WiFiDriver> wifi_driver;
    std::shared_ptr<TxFrame> txFrame;
    uint32_t video_channel_id_be;
    uint32_t mavlink_channel_id_be;
    uint32_t udp_channel_id_be;

    Logger_t log;
    std::unique_ptr<std::thread> usb_tx_thread{nullptr};
    uint32_t link_id{7669206};
    SignalQualityCalculator rssi_calculator;
};

#endif // FPV_VR_WFBNG_LINK_H
