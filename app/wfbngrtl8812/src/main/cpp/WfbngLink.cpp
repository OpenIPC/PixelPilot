#include <jni.h>
#include "WfbngLink.hpp"
#include <string>
#include <android/log.h>

#include <iostream>
#include <span>
#include <list>
#include <cstdint>
#include <initializer_list>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include "libusb.h"
#include "wfb-ng/src/wifibroadcast.hpp"
#include "RxFrame.h"

#include <sstream>
#include <iostream>
#include <iomanip>
#include <thread>

#undef TAG
#define TAG "pixelpilot"

std::string uint8_to_hex_string(const uint8_t *v, const size_t s) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < s; i++) {
        ss << std::hex << std::setw(2) << static_cast<int>(v[i]);
    }
    return ss.str();
}

WfbngLink::WfbngLink(JNIEnv *env, jobject context) {
    initAgg();
    Logger_t log;
    wifi_driver = std::make_unique<WiFiDriver>(log);
}

void WfbngLink::initAgg() {
    // TODO(geehe) Get that from the android UI.
    int video_client_port = 5600;
    int mavlink_client_port = 14550;
    std::string client_addr = "127.0.0.1";
    uint32_t link_id = 7669206; // sha1 hash of link_domain="default"
    uint8_t video_radio_port = 0;
    uint8_t mavlink_radio_port = 0x10;
    uint64_t epoch = 0;

    uint32_t video_channel_id_f = (link_id << 8) + video_radio_port;
    video_channel_id_be = htobe32(video_channel_id_f);
    uint32_t mavlink_channel_id_f = (link_id << 8) + mavlink_radio_port;
    mavlink_channel_id_be = htobe32(mavlink_channel_id_f);

    video_aggregator = std::make_unique<Aggregator>(client_addr, video_client_port, keyPath, epoch,
                                                    video_channel_id_f);
    mavlink_aggregator = std::make_unique<Aggregator>(client_addr, mavlink_client_port, keyPath,
                                                      epoch, mavlink_channel_id_f);
}

int WfbngLink::run(JNIEnv* env, jobject context, jint wifiChannel, jint bw, jint fd) {
    int r;
    libusb_context *ctx = NULL;

    r = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);

    r = libusb_init(&ctx);
    if (r < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to init libusb.");
        return r;
    }

    // Open adapters
    struct libusb_device_handle *dev_handle;
    r = libusb_wrap_sys_device(ctx, (intptr_t) fd, &dev_handle);
    if (r < 0) {
        libusb_exit(ctx);
        return r;
    }

    /*Check if kernel driver attached*/
    if (libusb_kernel_driver_active(dev_handle, 0)) {
        r = libusb_detach_kernel_driver(dev_handle, 0); // detach driver
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "libusb_detach_kernel_driver: %d", r);
    }
    r = libusb_claim_interface(dev_handle, 0);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Creating driver and device for fd=%d", fd);

    rtl_devices.emplace(fd, wifi_driver->CreateRtlDevice(dev_handle));
    if (!rtl_devices.at(fd)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "CreateRtlDevice error");
        return -1;
    }

    uint8_t *video_channel_id_be8 = reinterpret_cast<uint8_t *>(&video_channel_id_be);
    uint8_t *mavlink_channel_id_be8 = reinterpret_cast<uint8_t *>(&mavlink_channel_id_be);

    try {
        auto packetProcessor = [this, video_channel_id_be8, mavlink_channel_id_be8](
                const Packet &packet) {
            RxFrame frame(packet.Data);
            if (!frame.IsValidWfbFrame()) {
                return;
            }
            // TODO(geehe) Get data from libusb?
            int8_t rssi[4] = {1, 1, 1, 1};
            uint32_t freq = 0;
            int8_t noise[4] = {1, 1, 1, 1};
            uint8_t antenna[4] = {1, 1, 1, 1};

            std::lock_guard<std::mutex> lock(agg_mutex);
            if (frame.MatchesChannelID(video_channel_id_be8)) {
                video_aggregator->process_packet(packet.Data.data() + sizeof(ieee80211_header),
                                                 packet.Data.size() - sizeof(ieee80211_header) - 4,
                                                 0, antenna, rssi, noise, freq, 0, 0, NULL);
            } else if (frame.MatchesChannelID(mavlink_channel_id_be8)) {
                mavlink_aggregator->process_packet(packet.Data.data() + sizeof(ieee80211_header),
                                                   packet.Data.size() - sizeof(ieee80211_header) -
                                                   4, 0, antenna, rssi, noise, freq, 0, 0, NULL);
            }
        };

        auto bandWidth = (bw == 20 ? CHANNEL_WIDTH_20 : CHANNEL_WIDTH_40);
        rtl_devices.at(fd)->Init(packetProcessor,
                                 SelectedChannel{
                                     .Channel = static_cast<uint8_t>(wifiChannel),
                                     .ChannelOffset = 0,
                                     .ChannelWidth = bandWidth,
                                 });

    } catch (const std::runtime_error &error) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "runtime_error: %s", error.what());
        return -1;
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Init done, releasing...");

    r = libusb_release_interface(dev_handle, 0);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "libusb_release_interface: %d", r);

    libusb_exit(ctx);
    return 0;
}


void WfbngLink::stop(JNIEnv *env, jobject context, jint fd) {
    auto dev = rtl_devices.at(fd).get();
    if (dev) { dev->should_stop = true; }
}


//----------------------------------------------------JAVA bindings---------------------------------------------------------------
inline jlong jptr(WfbngLink *wfbngLinkN) {
    return reinterpret_cast<intptr_t>(wfbngLinkN);
}

inline WfbngLink *native(jlong ptr) {
    return reinterpret_cast<WfbngLink *>(ptr);
}

inline std::list<int> toList(JNIEnv *env, jobject list) {
    // Get the class and method IDs for java.util.List and its methods
    jclass listClass = env->GetObjectClass(list);
    jmethodID sizeMethod = env->GetMethodID(listClass, "size", "()I");
    jmethodID getMethod = env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;");
    // Method ID to get int value from Integer object
    jclass integerClass = env->FindClass("java/lang/Integer");
    jmethodID intValueMethod = env->GetMethodID(integerClass, "intValue", "()I");

    // Get the size of the list
    jint size = env->CallIntMethod(list, sizeMethod);

    // Create a C++ list to store the elements
    std::list<int> res;

    // Iterate over the list and add elements to the C++ list
    for (int i = 0; i < size; ++i) {
        // Get the element at index i
        jobject element = env->CallObjectMethod(list, getMethod, i);
        // Convert the element to int
        jint value = env->CallIntMethod(element, intValueMethod);
        // Add the element to the C++ list
        res.push_back(value);
    }

    return res;
}


extern "C" JNIEXPORT jlong JNICALL
Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeInitialize(JNIEnv *env, jclass clazz,
                                                         jobject context) {
    auto *p = new WfbngLink(env, context);
    return jptr(p);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRun(
    JNIEnv* env, jclass clazz, jlong wfbngLinkN, jobject androidContext, jint wifiChannel, jint bw, jint fd) {
    native(wfbngLinkN)->run(env, androidContext, wifiChannel, bw, fd);
}

extern "C" JNIEXPORT void JNICALL
Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStop(JNIEnv *env, jclass clazz, jlong wfbngLinkN,
                                                   jobject androidContext, jint fd) {
    native(wfbngLinkN)->stop(env, androidContext, fd);
}

extern "C" JNIEXPORT void JNICALL
Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeCallBack(JNIEnv *env, jclass clazz,
                                                       jobject wfbStatChangedI, jlong wfbngLinkN) {
    if (native(wfbngLinkN)->video_aggregator == nullptr) {
        return;
    }
    auto aggregator = native(wfbngLinkN)->video_aggregator.get();

    jclass jClassExtendsIWfbStatChangedI = env->GetObjectClass(wfbStatChangedI);
    jclass jcStats = env->FindClass("com/openipc/wfbngrtl8812/WfbNGStats");
    if (jcStats == nullptr) {
        return;
    }
    jmethodID jcStatsConstructor = env->GetMethodID(jcStats, "<init>", "(IIIIIIII)V");
    if (jcStatsConstructor == nullptr) {
        return;
    }
    auto stats = env->NewObject(jcStats, jcStatsConstructor,
                                (jint) aggregator->count_p_all,
                                (jint) aggregator->count_p_dec_err,
                                (jint) aggregator->count_p_dec_ok,
                                (jint) aggregator->count_p_fec_recovered,
                                (jint) aggregator->count_p_lost,
                                (jint) aggregator->count_p_bad,
                                (jint) aggregator->count_p_override,
                                (jint) aggregator->count_p_outgoing);
    if (stats == nullptr) {
        return;
    }
    jmethodID onStatsChanged = env->GetMethodID(jClassExtendsIWfbStatChangedI,
                                                "onWfbNgStatsChanged",
                                                "(Lcom/openipc/wfbngrtl8812/WfbNGStats;)V");
    if (onStatsChanged == nullptr) {
        return;
    }
    env->CallVoidMethod(wfbStatChangedI, onStatsChanged, stats);
    aggregator->clear_stats();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRefreshKey(JNIEnv *env, jclass clazz,
                                                         jlong wfbngLinkN) {
    native(wfbngLinkN)->initAgg();
}
