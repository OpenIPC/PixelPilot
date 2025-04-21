#ifndef FPV_VR_WFBNG_LINK_H
#define FPV_VR_WFBNG_LINK_H

extern "C" {
#include "wfb-ng/src/fec.h"
}

#include "SignalQualityCalculator.h"
#include "devourer/src/WiFiDriver.h"
#include "wfb-ng/src/rx.hpp"
#include <jni.h>
#include <list>
#include <map>
#include <mutex>

const u8 wfb_tx_port = 160;
const u8 wfb_rx_port = 32;

class WfbngLink {
  public:
    WfbngLink(JNIEnv *env, jobject context);

    int run(JNIEnv *env, jobject androidContext, jint wifiChannel, jint bw, jint fd);

    void initAgg();

    void stop(JNIEnv *env, jobject androidContext, jint fd);

    std::mutex agg_mutex;
    std::unique_ptr<Aggregator> video_aggregator;
    std::unique_ptr<Aggregator> mavlink_aggregator;
    std::unique_ptr<Aggregator> udp_aggregator;

    void start_link_quality_thread(int fd);

    // adaptive link
    // TODO: move this to private section
    int current_fd;
    bool adaptive_link_enabled;
    bool adaptive_link_should_stop{false};
    int adaptive_tx_power;
    std::map<int, std::unique_ptr<Rtl8812aDevice>> rtl_devices;
    std::unique_ptr<std::thread> link_quality_thread{nullptr};
    bool should_clear_stats{false};

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
    const char *keyPath = "/data/user/0/com.openipc.pixelpilot/files/gs.key";
    std::recursive_mutex thread_mutex;
    std::unique_ptr<WiFiDriver> wifi_driver;
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
