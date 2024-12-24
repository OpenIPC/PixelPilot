#ifndef FPV_VR_WFBNG_LINK_H
#define FPV_VR_WFBNG_LINK_H

extern "C" {
     #include "wfb-ng/src/fec.h"
}

#include <jni.h>
#include <list>
#include <map>
#include "wfb-ng/src/rx.hpp"
#include "devourer/src/WiFiDriver.h"

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

};

#endif //FPV_VR_WFBNG_LINK_H
