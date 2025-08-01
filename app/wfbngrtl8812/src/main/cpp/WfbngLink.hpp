#ifndef FPV_VR_WFBNG_LINK_H
#define FPV_VR_WFBNG_LINK_H

#include "WfbConfiguration.hpp"
#include "FecChangeController.h"
#include "SignalQualityCalculator.h"
#include "TxFrame.h"

extern "C" {
#include "wfb-ng/src/fec.h"
}

#include "devourer/src/WiFiDriver.h"
#include "wfb-ng/src/rx.hpp"

// Forward declarations
class DeviceManager;
class AggregatorManager;
class ThreadManager;
class AdaptiveLinkController;
class PacketProcessor;
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
    WfbngLink(JNIEnv *env, jobject context, const WfbConfiguration& config = WfbConfiguration::createDefault());

    int run(JNIEnv *env, jobject androidContext, jint wifiChannel, jint bw, jint fd);

    void initAgg();

    void stop(JNIEnv *env, jobject androidContext, jint fd);

    // Configuration methods
    void setFecThresholds(int lostTo5, int recTo4, int recTo3, int recTo2, int recTo1);

    // Adaptive link control methods
    void setAdaptiveLinkEnabled(bool enabled);
    void setAdaptiveTxPower(int power);

    // Legacy public access for JNI compatibility
    std::unique_ptr<AggregatorManager> aggregator_manager;
    std::unique_ptr<DeviceManager> device_manager;
    FecChangeController fec;

    // Runtime configurable PHY parameters
    bool ldpc_enabled{true};
    bool stbc_enabled{true};

    // For backward compatibility with JNI layer
    int current_fd{-1};
    bool adaptive_link_enabled{true};
    int adaptive_tx_power{30};
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

    void stop_adaptive_link();

  private:
    void stopDevice();

    WfbConfiguration config_;

    // Component managers
    std::unique_ptr<ThreadManager> thread_manager_;
    std::unique_ptr<AdaptiveLinkController> adaptive_controller_;
    std::unique_ptr<PacketProcessor> packet_processor_;

    // Legacy members (to be removed after full migration)
    const char *keyPath = "/data/user/0/com.openipc.pixelpilot/files/gs.key";
    std::recursive_mutex thread_mutex;
    std::shared_ptr<WiFiDriver> wifi_driver;
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
