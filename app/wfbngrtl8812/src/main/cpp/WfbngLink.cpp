#include "WfbngLink.hpp"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>

#include "RxFrame.h"
#include "SignalQualityCalculator.h"
#include "TxFrame.h"
#include "libusb.h"
#include "wfb-ng/src/wifibroadcast.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#undef TAG
#define TAG "pixelpilot"

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

WfbngLink::WfbngLink(JNIEnv *env, jobject context)
        : current_fd(-1), adaptive_link_enabled(true), adaptive_tx_power(30) {
    initAgg();
    Logger_t log;
    wifi_driver = std::make_unique<WiFiDriver>(log);
}

void WfbngLink::initAgg() {
    std::string client_addr = "127.0.0.1";
    uint64_t epoch = 0;

    uint8_t video_radio_port = 0;
    uint32_t video_channel_id_f = (link_id << 8) + video_radio_port;
    video_channel_id_be = htobe32(video_channel_id_f);
    auto udsName = std::string("my_socket");

    video_aggregator = std::make_unique<AggregatorUNIX>(udsName, keyPath, epoch, video_channel_id_f, 0);

    int mavlink_client_port = 14550;
    uint8_t mavlink_radio_port = 0x10;
    uint32_t mavlink_channel_id_f = (link_id << 8) + mavlink_radio_port;
    mavlink_channel_id_be = htobe32(mavlink_channel_id_f);

    mavlink_aggregator =
        std::make_unique<AggregatorUDPv4>(client_addr, mavlink_client_port, keyPath, epoch, mavlink_channel_id_f, 0);

    int udp_client_port = 8000;
    uint8_t udp_radio_port = wfb_rx_port;
    uint32_t udp_channel_id_f = (link_id << 8) + udp_radio_port;
    udp_channel_id_be = htobe32(udp_channel_id_f);

    udp_aggregator =
        std::make_unique<AggregatorUDPv4>(client_addr, udp_client_port, keyPath, epoch, udp_channel_id_f, 0);
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

    rtl_devices.emplace(fd, wifi_driver->CreateRtlDevice(dev_handle));
    if (!rtl_devices.at(fd)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "CreateRtlDevice error");
        return -1;
    }

    uint8_t *video_channel_id_be8 = reinterpret_cast<uint8_t *>(&video_channel_id_be);
    uint8_t *udp_channel_id_be8 = reinterpret_cast<uint8_t *>(&udp_channel_id_be);
    uint8_t *mavlink_channel_id_be8 = reinterpret_cast<uint8_t *>(&mavlink_channel_id_be);

    try {
        auto packetProcessor =
            [this, video_channel_id_be8, mavlink_channel_id_be8, udp_channel_id_be8](const Packet &packet) {
                RxFrame frame(packet.Data);
                if (!frame.IsValidWfbFrame()) {
                    return;
                }
                int8_t rssi[4] = {(int8_t)packet.RxAtrib.rssi[0], (int8_t)packet.RxAtrib.rssi[1], 1, 1};
                uint32_t freq = 0;
                int8_t noise[4] = {1, 1, 1, 1};
                uint8_t antenna[4] = {1, 1, 1, 1};

                std::lock_guard<std::mutex> lock(agg_mutex);
                if (frame.MatchesChannelID(video_channel_id_be8)) {
                    SignalQualityCalculator::get_instance().add_rssi(packet.RxAtrib.rssi[0], packet.RxAtrib.rssi[1]);
                    SignalQualityCalculator::get_instance().add_snr(packet.RxAtrib.snr[0], packet.RxAtrib.snr[1]);

                    video_aggregator->process_packet(packet.Data.data() + sizeof(ieee80211_header),
                                                     packet.Data.size() - sizeof(ieee80211_header) - 4,
                                                     0,
                                                     antenna,
                                                     rssi,
                                                     noise,
                                                     freq,
                                                     0,
                                                     0,
                                                     NULL);
                    if (should_clear_stats) {
                        video_aggregator->clear_stats();
                        should_clear_stats = false;
                    }
                } else if (frame.MatchesChannelID(mavlink_channel_id_be8)) {
                    mavlink_aggregator->process_packet(packet.Data.data() + sizeof(ieee80211_header),
                                                       packet.Data.size() - sizeof(ieee80211_header) - 4,
                                                       0,
                                                       antenna,
                                                       rssi,
                                                       noise,
                                                       freq,
                                                       0,
                                                       0,
                                                       NULL);
                } else if (frame.MatchesChannelID(udp_channel_id_be8)) {
                    udp_aggregator->process_packet(packet.Data.data() + sizeof(ieee80211_header),
                                                   packet.Data.size() - sizeof(ieee80211_header) - 4,
                                                   0,
                                                   antenna,
                                                   rssi,
                                                   noise,
                                                   freq,
                                                   0,
                                                   0,
                                                   NULL);
                }
            };

        // Store the current fd for later TX power updates.
        current_fd = fd;

        if (!usb_event_thread) {
            auto usb_event_thread_func = [ctx, this, fd] {
                while (true) {
                    auto dev = this->rtl_devices.at(fd).get();
                    if (dev == nullptr || dev->should_stop) break;
                    struct timeval timeout = {0, 500000}; // 500ms timeout
                    int r = libusb_handle_events_timeout(ctx, &timeout);
                    if (r < 0) {
                        this->log->error("Error handling events: {}", r);
                        // break;
                    }
                }
            };

            init_thread(usb_event_thread, [=]() { return std::make_unique<std::thread>(usb_event_thread_func); });

            std::shared_ptr<TxArgs> args = std::make_shared<TxArgs>();
            args->udp_port = 8001;
            args->link_id = link_id;
            args->keypair = keyPath;
            args->stbc = true;
            args->ldpc = true;
            args->mcs_index = 0;
            args->vht_mode = false;
            args->short_gi = false;
            args->bandwidth = 20;
            args->k = 1;
            args->n = 5;
            args->radio_port = wfb_tx_port;

            __android_log_print(
                ANDROID_LOG_ERROR, TAG, "radio link ID %d, radio PORT %d", args->link_id, args->radio_port);

            Rtl8812aDevice *current_device = rtl_devices.at(fd).get();
            if (!usb_tx_thread) {
                init_thread(usb_tx_thread, [&]() {
                    return std::make_unique<std::thread>([this, current_device, args] {
                        txFrame->run(current_device, args.get());
                        __android_log_print(ANDROID_LOG_DEBUG, TAG, "usb_transfer thread should terminate");
                    });
                });
            }

            if (adaptive_link_enabled) {
                stop_adaptive_link();
                start_link_quality_thread(fd);
            }
        }

        auto bandWidth = (bw == 20 ? CHANNEL_WIDTH_20 : CHANNEL_WIDTH_40);
        rtl_devices.at(fd)->Init(packetProcessor,
                                 SelectedChannel{
                                     .Channel = static_cast<uint8_t>(wifiChannel),
                                     .ChannelOffset = 0,
                                     .ChannelWidth = bandWidth,
                                 });
    } catch (const std::runtime_error &error) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "runtime_error: %s", error.what());
        auto dev = rtl_devices.at(fd).get();
        if (dev) {
            dev->should_stop = true;
        }
        txFrame->stop();

        destroy_thread(usb_tx_thread);
        destroy_thread(usb_event_thread);
        stop_adaptive_link();
        return -1;
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Init done, releasing...");
    auto dev = rtl_devices.at(fd).get();
    if (dev) {
        dev->should_stop = true;
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
    if (rtl_devices.find(fd) == rtl_devices.end()) return;
    auto dev = rtl_devices.at(fd).get();
    if (dev) {
        dev->should_stop = true;
    }
    stop_adaptive_link();
}

//--------------------------------------JAVA bindings--------------------------------------
inline jlong jptr(WfbngLink *wfbngLinkN) { return reinterpret_cast<intptr_t>(wfbngLinkN); }

inline WfbngLink *native(jlong ptr) { return reinterpret_cast<WfbngLink *>(ptr); }

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
extern "C" JNIEXPORT jlong JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeInitialize(JNIEnv *env,
                                                                                            jclass clazz,
                                                                                            jobject context) {
    auto *p = new WfbngLink(env, context);
    return jptr(p);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRun(
    JNIEnv *env, jclass clazz, jlong wfbngLinkN, jobject androidContext, jint wifiChannel, int bandWidth, jint fd) {
    native(wfbngLinkN)->run(env, androidContext, wifiChannel, bandWidth, fd);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStartAdaptivelink(JNIEnv *env,
                                                                                                  jclass clazz,
                                                                                                  jlong wfbngLinkN) {
    if (native(wfbngLinkN)->video_aggregator == nullptr) {
        return;
    }
    auto aggregator = native(wfbngLinkN)->video_aggregator.get();
}

extern "C" JNIEXPORT jint JNICALL Java_com_openipc_pixelpilot_UsbSerialService_nativeGetSignalQuality(JNIEnv *env,
                                                                                                      jclass clazz) {
    return SignalQualityCalculator::get_instance().calculate_signal_quality().quality;
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStop(
    JNIEnv *env, jclass clazz, jlong wfbngLinkN, jobject androidContext, jint fd) {
    native(wfbngLinkN)->stop(env, androidContext, fd);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeCallBack(JNIEnv *env,
                                                                                         jclass clazz,
                                                                                         jobject wfbStatChangedI,
                                                                                         jlong wfbngLinkN) {
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
    SignalQualityCalculator::get_instance().add_fec_data(
        aggregator->count_p_all, aggregator->count_p_fec_recovered, aggregator->count_p_lost);
    auto stats = env->NewObject(jcStats,
                                jcStatsConstructor,
                                (jint)aggregator->count_p_all,
                                (jint)aggregator->count_p_dec_err,
                                (jint)(aggregator->count_p_all - aggregator->count_p_dec_err),
                                (jint)aggregator->count_p_fec_recovered,
                                (jint)aggregator->count_p_lost,
                                (jint)aggregator->count_p_bad,
                                (jint)aggregator->count_p_override,
                                (jint)aggregator->count_p_outgoing);
    if (stats == nullptr) {
        return;
    }
    jmethodID onStatsChanged = env->GetMethodID(
        jClassExtendsIWfbStatChangedI, "onWfbNgStatsChanged", "(Lcom/openipc/wfbngrtl8812/WfbNGStats;)V");
    if (onStatsChanged == nullptr) {
        return;
    }
    env->CallVoidMethod(wfbStatChangedI, onStatsChanged, stats);
    native(wfbngLinkN)->should_clear_stats = true;
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRefreshKey(JNIEnv *env,
                                                                                           jclass clazz,
                                                                                           jlong wfbngLinkN) {
    native(wfbngLinkN)->initAgg();
}

// Modified start_link_quality_thread: use adaptive_link_enabled and adaptive_tx_power
void WfbngLink::start_link_quality_thread(int fd) {
    auto thread_func = [this, fd]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const char *ip = "10.5.0.10";
        int port = 9999;
        int sockfd;
        struct sockaddr_in server_addr;
        // Create UDP socket
        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Socket creation failed");
            return;
        }
        int opt = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid IP address");
            close(sockfd);
            return;
        }
        int sockfd2;
        struct sockaddr_in server_addr2;
        // Create UDP socket
        if ((sockfd2 = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Socket creation failed");
            return;
        }
        int opt2 = 1;
        setsockopt(sockfd2, SOL_SOCKET, SO_REUSEADDR, &opt2, sizeof(opt2));
        memset(&server_addr2, 0, sizeof(server_addr2));
        server_addr2.sin_family = AF_INET;
        server_addr2.sin_port = htons(7755);
        if (inet_pton(AF_INET, ip, &server_addr2.sin_addr) <= 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid IP address");
            close(sockfd);
            return;
        }
        while (!this->adaptive_link_should_stop) {
            auto quality = SignalQualityCalculator::get_instance().calculate_signal_quality();
#if defined(ANDROID_DEBUG_RSSI) || true
            __android_log_print(ANDROID_LOG_WARN, TAG, "quality %d", quality.quality);
#endif
            time_t currentEpoch = time(nullptr);
            const auto map_range =
                [](double value, double inputMin, double inputMax, double outputMin, double outputMax) {
                    return outputMin + ((value - inputMin) * (outputMax - outputMin) / (inputMax - inputMin));
                };
            // map to 1000..2000
            quality.quality = map_range(quality.quality, -1024, 1024, 1000, 2000);
            {
                uint32_t len;
                char message[100];

                /**
                     1741491090:1602:1602:1:0:-70:24:num_ants:pnlt:fec_change:code

                     <gs_time>:<link_score>:<link_score>:<fec>:<lost>:<rssi_dB>:<snr_dB>:<num_ants>:<noise_penalty>:<fec_change>:<idr_request_code>

                    gs_time: gs clock
                    link_score: 1000 - 2000 sent twice (already including any penalty)
                    link_score: 1000 - 2000 sent twice (already including any penalty)
                    fec: instantaneus fec_rec (only used by old fec_rec_pntly now disabled by default)
                    lost: instantaneus lost (not used)
                    rssi_dB:  best antenna rssi (for osd)
                    snr_dB: best antenna snr_dB (for osd)
                    num_ants: number of gs antennas (for osd)
                    noise_penalty: penalty deducted from score due to noise (for osd)
                    fec_change: int from 0 to 5 : how much to alter fec based on noise
                    optional idr_request_code:  4 char unique code to request 1 keyframe (no need to send special extra
                   packets)
                 */
                snprintf(message + sizeof(len),
                         sizeof(message) - sizeof(len),
                         "%ld:%d:%d:%d:%d:%d:%f:0:-1:0:%s\n",
                         static_cast<long>(currentEpoch),
                         quality.quality,
                         quality.quality,
                         quality.recovered_last_second,
                         quality.lost_last_second,
                         quality.quality,
                         quality.snr,
                         quality.idr_code.c_str());

                len = strlen(message + sizeof(len));
                len = htonl(len);
                memcpy(message, &len, sizeof(len));
                __android_log_print(ANDROID_LOG_ERROR, TAG, "message %s", message + 4);
                ssize_t sent = sendto(sockfd,
                                      message,
                                      strlen(message + sizeof(len)) + sizeof(len),
                                      0,
                                      (struct sockaddr *)&server_addr,
                                      sizeof(server_addr));
                if (sent < 0) {
                    __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to send message");
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        close(sockfd);
        this->adaptive_link_should_stop = false;
    };

    init_thread(link_quality_thread, [=]() { return std::make_unique<std::thread>(thread_func); });
    rtl_devices.at(fd)->SetTxPower(adaptive_tx_power);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetAdaptiveLinkEnabled(
    JNIEnv *env, jclass clazz, jlong wfbngLinkN, jboolean enabled) {
    WfbngLink *link = native(wfbngLinkN);
    bool wasEnabled = link->adaptive_link_enabled;
    link->adaptive_link_enabled = enabled;
    // If we are enabling adaptive mode (and it was previously disabled)
    if (enabled && !wasEnabled) {
        link->stop_adaptive_link();
        if (link->current_fd != -1) {
            // If a previous adaptive thread exists, join it first.
            // Restart the adaptive (link quality) thread.
            link->start_link_quality_thread(link->current_fd);
        }
    }
    // When disabling, wait for the thread to exit (if running).
    if (!enabled && wasEnabled) {
        link->stop_adaptive_link();
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetTxPower(JNIEnv *env,
                                                                                           jclass clazz,
                                                                                           jlong wfbngLinkN,
                                                                                           jint power) {
    WfbngLink *link = native(wfbngLinkN);
    if (link->adaptive_tx_power == power) return;

    link->adaptive_tx_power = power;
    if (link->current_fd != -1 && link->rtl_devices.find(link->current_fd) != link->rtl_devices.end()) {
        link->rtl_devices.at(link->current_fd)->SetTxPower(power);
    }
    // If adaptive mode is enabled and the adaptive thread is not running, restart it.
    if (link->adaptive_link_enabled) {
        link->stop_adaptive_link();
        if (link->current_fd != -1) {
            link->start_link_quality_thread(link->current_fd);
        }
    }
}
