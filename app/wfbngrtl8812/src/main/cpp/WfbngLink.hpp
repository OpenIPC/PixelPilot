#ifndef FPV_VR_WFBNG_LINK_H
#define FPV_VR_WFBNG_LINK_H

#include "FecChangeController.h"
#include "SignalQualityCalculator.h"
#include "TxFrame.h"

extern "C" {
#include "wfb-ng/src/zfex.h"
}

#include "devourer/src/WiFiDriver.h"
#include "wfb-ng/src/rx.hpp"
#include <jni.h>
#include <list>
#include <map>
#include <mutex>
#include <vector> // Added for std::vector

const u8 wfb_tx_port = 160;
const u8 wfb_rx_port = 32;

class WfbngLink {
  public:
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

    std::map<int, std::shared_ptr<Rtl8812aDevice>> rtl_devices;
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
    void stopDevice() {
        if (rtl_devices.find(current_fd) == rtl_devices.end()) return;
        auto dev = rtl_devices.at(current_fd).get();
        if (dev) {
            dev->should_stop = true;
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
    std::unique_ptr<std::thread> usb_event_thread{nullptr};
    std::unique_ptr<std::thread> usb_tx_thread{nullptr};
    uint32_t link_id{7669206};
    SignalQualityCalculator rssi_calculator;
};

#endif // FPV_VR_WFBNG_LINK_H
