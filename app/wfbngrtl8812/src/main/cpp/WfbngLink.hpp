#ifndef FPV_VR_WFBNG_LINK_H
#define FPV_VR_WFBNG_LINK_H

extern "C" {
     #include "wfb-ng/src/fec.h"
}

#include <jni.h>
#include <list>
#include <map>
#include <mutex>
#include "wfb-ng/src/rx.hpp"
#include "devourer/src/WiFiDriver.h"

class RssiCalculator{
public:
    void add_rssi(uint8_t ant1, uint8_t ant2);
    float get_avg_rssi();

private:
    std::mutex rssis_mutex;
    std::vector<std::pair<uint8_t, uint8_t>> rssis;
};

const u8 wfb_tx_port = 160;
const u8 wfb_rx_port = 32;

class WfbngLink {
public:
    WfbngLink(JNIEnv *env, jobject context);

    int run(JNIEnv *env, jobject androidContext, jint wifiChannel, jint fd);

    void initAgg();

    void stop(JNIEnv *env, jobject androidContext, jint fd);

    std::mutex agg_mutex;
    std::unique_ptr<Aggregator> video_aggregator;
    std::unique_ptr<Aggregator> mavlink_aggregator;
    std::unique_ptr<Aggregator> udp_aggregator;

private:
    const char *keyPath = "/data/user/0/com.openipc.pixelpilot/files/gs.key";
    std::unique_ptr<WiFiDriver> wifi_driver;
    uint32_t video_channel_id_be;
    uint32_t mavlink_channel_id_be;
    uint32_t udp_channel_id_be;
    Logger_t log;
    std::map<int, std::unique_ptr<Rtl8812aDevice>> rtl_devices;
    std::unique_ptr<std::thread> usb_event_thread {nullptr};
    std::unique_ptr<std::thread> usb_tx_thread{nullptr};
    std::unique_ptr<std::thread> link_quality_thread{nullptr};
    uint32_t link_id{7669206};
    RssiCalculator rssi_calculator;
};

#endif //FPV_VR_WFBNG_LINK_H
