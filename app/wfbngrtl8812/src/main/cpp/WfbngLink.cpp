#include "WfbngLink.hpp"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>

#include "RxFrame.h"
#include "SignalQualityCalculator.h"
#include "TxFrame.h"
#include "devourer/src/RxPacket.h"
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
    log = std::make_shared<Logger>(); // routes to logcat under the "devourer" tag
    wifi_driver = std::make_unique<WiFiDriver>(log);
}

void WfbngLink::initAgg() {
    std::string client_addr = "127.0.0.1";
    uint64_t epoch = 0;

    uint8_t video_radio_port = 0;
    uint32_t video_channel_id_f = (link_id << 8) + video_radio_port;
    video_channel_id_be = htobe32(video_channel_id_f);
    auto udsName = std::string("my_socket");

    video_aggregator = std::make_unique<AggregatorUDPv4>(client_addr, 5600, keyPath, epoch, video_channel_id_f, 0);

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
    clear_stop_request(fd);
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

    devourer::DeviceConfig cfg;
    // TX+RX on the one claimed handle. Jaguar3 (RTL8812EU/8822EU) must know
    // this before InitWrite so the bring-up keeps the RX filters open;
    // Jaguar1 (RTL8812AU) ignores it.
    cfg.rx.enable_with_tx = true;
    // The per-adapter advisory lock defaults to /tmp, which doesn't exist on
    // Android — use the app's files dir (same location as gs.key).
    cfg.usb.lock_dir = "/data/user/0/com.openipc.pixelpilot/files";
    // Keep the RX ring on plain heap buffers. The zerocopy dev-mem path
    // (libusb_dev_mem_alloc / USBDEVFS mmap) is unvalidated on Android vendor
    // kernels; if the mmap succeeds but the HCD's zerocopy path is broken,
    // the heap fallback never triggers. Heap is the behavior every working
    // Android build has shipped.
    cfg.usb.rx_zerocopy = false;
    const bool ap_mode = link_mode.load() == LinkMode::AP;
    if (ap_mode) {
        // A station's unicast frames to the AP are acknowledged, so let the chip
        // retransmit until they are (wfb-ng mode relies on FEC and sends once).
        cfg.tx.retry_limit = 7;
    }

    // The local copy is dropped (device.reset()) before libusb is torn down below.
    std::shared_ptr<IRtlDevice> device = wifi_driver->CreateRtlDevice(dev_handle, ctx, nullptr, cfg);
    if (!device) {
        libusb_exit(ctx);
        __android_log_print(ANDROID_LOG_ERROR, TAG, "CreateRtlDevice error");
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(dev_mutex);
        rtl_devices[fd] = device;
    }

    if (stop_requested(fd)) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "stop requested for fd=%d before bring-up, aborting", fd);
        releaseDevice(fd);
        device.reset();
        libusb_release_interface(dev_handle, 0);
        libusb_exit(ctx);
        return -1;
    }

    if (ap_mode) {
        return runStation(dev_handle, ctx, fd, wifiChannel, std::move(device));
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

        IRtlDevice *current_device = device.get();

        // TX-capable bring-up with RX enabled (cfg.rx.enable_with_tx). On
        // Jaguar3 (RTL8812EU/8822EU) this also starts the coex runtime thread
        // that sustained TX needs.
        auto bandWidth = (bw == 20 ? CHANNEL_WIDTH_20 : CHANNEL_WIDTH_40);
        current_device->InitWrite(SelectedChannel{
            .Channel = static_cast<uint8_t>(wifiChannel),
            .ChannelOffset = 0,
            .ChannelWidth = bandWidth,
        });

        if (!usb_tx_thread) {
            std::shared_ptr<TxArgs> args = std::make_shared<TxArgs>();
            args->udp_port = 8001;
            args->link_id = link_id;
            args->keypair = keyPath;
            args->stbc = stbc_enabled;
            args->ldpc = ldpc_enabled;
            args->mcs_index = 0;
            args->vht_mode = false;
            args->short_gi = false;
            args->bandwidth = 20;
            args->k = 1;
            args->n = 5;
            args->radio_port = wfb_tx_port;

            __android_log_print(
                ANDROID_LOG_ERROR, TAG, "radio link ID %d, radio PORT %d", args->link_id, args->radio_port);

            init_thread(usb_tx_thread, [&]() {
                return std::make_unique<std::thread>([this, current_device, args] {
                    txFrame->run(current_device, args.get());
                    __android_log_print(ANDROID_LOG_DEBUG, TAG, "usb_transfer thread should terminate");
                });
            });

            if (adaptive_link_enabled) {
                stop_adaptive_link();
                start_link_quality_thread(fd);
            }
        }

        // Blocking RX loop on this thread; devourer pumps the libusb events
        // itself. Returns once StopRxLoop() is called.
        if (stop_requested(fd)) {
            __android_log_print(
                ANDROID_LOG_WARN, TAG, "stop requested for fd=%d during bring-up, not entering the rx loop", fd);
        } else {
            current_device->StartRxLoop(packetProcessor);
        }
    } catch (const std::runtime_error &error) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "runtime_error: %s", error.what());
        txFrame->stop();

        destroy_thread(usb_tx_thread);
        stop_adaptive_link();
        if (device) {
            device->Stop();
        }
        releaseDevice(fd);
        device.reset(); // before libusb_exit(): the device must not outlive its context
        libusb_release_interface(dev_handle, 0);
        libusb_exit(ctx);
        return -1;
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "RX loop exited, releasing...");
    txFrame->stop();

    destroy_thread(usb_tx_thread);
    stop_adaptive_link();

    // Clean shutdown: halt TRX DMA and power the chip down before releasing
    // the USB interface.
    if (device) {
        device->Stop();
    }
    releaseDevice(fd);
    device.reset(); // before libusb_exit(): the device must not outlive its context

    r = libusb_release_interface(dev_handle, 0);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "libusb_release_interface: %d", r);
    libusb_exit(ctx);
    return 0;
}

int WfbngLink::runStation(libusb_device_handle *dev_handle,
                          libusb_context *ctx,
                          int fd,
                          int wifiChannel,
                          std::shared_ptr<IRtlDevice> device) {
    IRtlDevice *dev = device.get();

    StaLink *sta = nullptr;
    devourer::MacAddr own_mac;
    {
        std::lock_guard<std::mutex> lock(sta_mutex);
        if (sta_link) {
            // One association per AP; a second adapter would only fight the first over the same MAC.
            __android_log_print(ANDROID_LOG_WARN, TAG, "AP mode already runs on another adapter, ignoring fd=%d", fd);
        } else if (sta_config.ssid.empty()) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "AP mode without an SSID configured, not starting");
        } else {
            StaConfig cfg = sta_config;
            cfg.channel = static_cast<uint8_t>(wifiChannel);
            own_mac.bytes = cfg.own_mac;
            sta_link = std::make_shared<StaLink>(dev, cfg);
            sta = sta_link.get();
            // Only the adapter that owns the association becomes the current one; a
            // rejected second adapter must not overwrite it.
            current_fd = fd;
        }
    }

    if (!sta) {
        // Never brought up, so nothing to power down; same exit as an early stop in run().
        releaseDevice(fd);
        device.reset();
        libusb_release_interface(dev_handle, 0);
        libusb_exit(ctx);
        return -1;
    }

    int ret = 0;
    try {
        // The camera's AP is joined on its primary channel at 20 MHz; HT40 APs
        // still talk to a 20 MHz-only station on the primary.
        dev->InitWrite(SelectedChannel{
            .Channel = static_cast<uint8_t>(wifiChannel),
            .ChannelOffset = 0,
            .ChannelWidth = CHANNEL_WIDTH_20,
        });
        dev->SetTxPower(adaptive_tx_power);

        // The chip ACKs every unicast frame addressed to own_mac within SIFS, which a
        // userspace stack could never do. Without it the AP retries each frame to us.
        if (!dev->SetAckResponder(own_mac)) {
            __android_log_print(
                ANDROID_LOG_WARN, TAG, "adapter has no hardware ACK responder, the AP will retry every frame");
        }

        if (stop_requested(fd)) {
            __android_log_print(
                ANDROID_LOG_WARN, TAG, "stop requested for fd=%d during bring-up, not entering the rx loop", fd);
        } else {
            sta->start();
            // Blocking until stop() calls StopRxLoop().
            dev->StartRxLoop([sta](const Packet &packet) { sta->onRx(packet); });
        }
    } catch (const std::runtime_error &error) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "runtime_error: %s", error.what());
        ret = -1;
    }

    // Stop before the device goes: a camera command may still hold a reference,
    // and a stopped StaLink no longer touches the device.
    sta->stop();
    {
        std::lock_guard<std::mutex> lock(sta_mutex);
        ap_recovery_base += sta_link->recoveries();
        sta_link.reset();
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "AP mode RX loop exited, releasing...");
    dev->Stop();
    releaseDevice(fd);
    device.reset(); // before libusb_exit(): the device must not outlive its context
    int r = libusb_release_interface(dev_handle, 0);
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "libusb_release_interface: %d", r);
    libusb_exit(ctx);
    return ret;
}

void WfbngLink::stop(JNIEnv *env, jobject context, jint fd) {
    // Recorded first, and whether or not the device exists yet: run() may not have got as far
    // as creating it, and StartRxLoop() would clear the flag set below anyway.
    note_stop_requested(fd);
    // A copy keeps the device alive while StopRxLoop() runs, even if run() releases it meanwhile.
    std::shared_ptr<IRtlDevice> dev = getDevice(fd);
    if (!dev) {
        // Happens when the adapter was already gone by the time the stop arrived, e.g. it
        // was unplugged or the hub re-enumerated it. Nothing left to stop.
        __android_log_print(ANDROID_LOG_WARN, TAG, "stop: no rtl device for fd=%d, already gone", fd);
        return;
    }
    dev->StopRxLoop();
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

float map_range(float value, float inputMin, float inputMax, float outputMin, float outputMax) {
    return outputMin + ((value - inputMin) * (outputMax - outputMin) / (inputMax - inputMin));
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
    jmethodID jcStatsConstructor = env->GetMethodID(jcStats, "<init>", "(IIIIIIIII)V");
    if (jcStatsConstructor == nullptr) {
        return;
    }

    jobject stats;
    if (native(wfbngLinkN)->link_mode.load() == LinkMode::AP) {
        // No FEC in AP mode: report data frames received, UDP datagrams handed to
        // the player, frames we had to drop and frames sent, in the same slots.
        StaLink::Stats st{};
        {
            std::lock_guard<std::mutex> lock(native(wfbngLinkN)->sta_mutex);
            if (native(wfbngLinkN)->sta_link) st = native(wfbngLinkN)->sta_link->takeStats();
        }
        auto quality = SignalQualityCalculator::get_instance().calculate_signal_quality();
        uint32_t avg_rssi_int = round(map_range(quality.quality, -1024.f, 1024.f, 0.f, 100.f));
        stats = env->NewObject(jcStats,
                               jcStatsConstructor,
                               (jint)st.rx_data,
                               (jint)0,
                               (jint)st.rx_udp,
                               (jint)0,
                               (jint)st.rx_dropped,
                               (jint)0,
                               (jint)0,
                               (jint)st.tx_frames,
                               (jint)avg_rssi_int);
    } else {
        SignalQualityCalculator::get_instance().add_fec_data(
            aggregator->count_p_all, aggregator->count_p_fec_recovered, aggregator->count_p_lost);

        auto quality = SignalQualityCalculator::get_instance().calculate_signal_quality();
        uint32_t avg_rssi_int = round(map_range(quality.quality, -1024.f, 1024.f, 0.f, 100.f));

        stats = env->NewObject(jcStats,
                               jcStatsConstructor,
                               (jint)aggregator->count_p_all,
                               (jint)aggregator->count_p_dec_err,
                               (jint)(aggregator->count_p_all - aggregator->count_p_dec_err),
                               (jint)aggregator->count_p_fec_recovered,
                               (jint)aggregator->count_p_lost,
                               (jint)aggregator->count_p_bad,
                               (jint)aggregator->count_p_override,
                               (jint)aggregator->count_p_outgoing,
                               (jint)avg_rssi_int);
    }
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

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetLinkMode(JNIEnv *env,
                                                                                          jclass clazz,
                                                                                          jlong wfbngLinkN,
                                                                                          jint mode) {
    native(wfbngLinkN)->link_mode = (mode == static_cast<int>(LinkMode::AP)) ? LinkMode::AP : LinkMode::WFB;
}

// Returns false (and keeps the previous config) when the IP or MAC does not parse.
extern "C" JNIEXPORT jboolean JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetApConfig(
    JNIEnv *env,
    jclass clazz,
    jlong wfbngLinkN,
    jstring ssid,
    jstring ownIp,
    jstring ownMac,
    jint videoPort,
    jintArray candidateChannels,
    jboolean fullScan,
    jint maxWidthMhz) {
    auto str = [env](jstring s) {
        const char *c = env->GetStringUTFChars(s, nullptr);
        std::string out(c ? c : "");
        if (c) env->ReleaseStringUTFChars(s, c);
        return out;
    };
    StaConfig cfg;
    if (candidateChannels != nullptr) {
        const jsize n = env->GetArrayLength(candidateChannels);
        std::vector<jint> chans(n);
        env->GetIntArrayRegion(candidateChannels, 0, n, chans.data());
        for (jint ch : chans) {
            if (ch > 0 && ch < 200) cfg.candidate_channels.push_back(static_cast<uint8_t>(ch));
        }
    }
    cfg.full_scan = fullScan;
    cfg.max_width_mhz = (maxWidthMhz >= 80) ? 80 : (maxWidthMhz >= 40) ? 40 : 20;
    cfg.ssid = str(ssid);
    if (cfg.ssid.size() > 32) return JNI_FALSE;
    if (inet_pton(AF_INET, str(ownIp).c_str(), cfg.own_ip.data()) != 1) return JNI_FALSE;
    auto mac = devourer::parse_mac(str(ownMac));
    // Must be unicast: the ACK engine cannot match a group address.
    if (!mac || (mac->bytes[0] & 0x01)) return JNI_FALSE;
    cfg.own_mac = mac->bytes;
    if (videoPort <= 0 || videoPort > 65535) return JNI_FALSE;
    // The player and the MAVLink reader listen on fixed local ports.
    cfg.forward_ports = {{static_cast<uint16_t>(videoPort), 5600}, {14550, 14550}};

    WfbngLink *link = native(wfbngLinkN);
    std::lock_guard<std::mutex> lock(link->sta_mutex);
    link->sta_config = std::move(cfg);
    return JNI_TRUE;
}

// Bytes from the air (SSIDs of other networks, camera replies) are not guaranteed
// to be valid modified UTF-8, which NewStringUTF() requires (CheckJNI aborts on it).
static std::string printableAscii(std::string s) {
    for (char &c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || (u < 0x20 && c != '\n' && c != '\r' && c != '\t')) c = '?';
    }
    return s;
}

extern "C" JNIEXPORT jstring JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeGetApStatus(JNIEnv *env,
                                                                                             jclass clazz,
                                                                                             jlong wfbngLinkN) {
    WfbngLink *link = native(wfbngLinkN);
    std::string text; // empty: no adapter running in AP mode
    {
        std::lock_guard<std::mutex> lock(link->sta_mutex);
        if (link->sta_link) text = link->sta_link->statusText();
    }
    return env->NewStringUTF(printableAscii(text).c_str());
}

extern "C" JNIEXPORT jint JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeGetApChannel(JNIEnv *env,
                                                                                           jclass clazz,
                                                                                           jlong wfbngLinkN) {
    WfbngLink *link = native(wfbngLinkN);
    std::lock_guard<std::mutex> lock(link->sta_mutex);
    return link->sta_link ? link->sta_link->connectedChannel() : 0;
}

// Grows each time video can flow again in AP mode (association, end of a dropout).
extern "C" JNIEXPORT jint JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeGetApRecoveries(JNIEnv *env,
                                                                                              jclass clazz,
                                                                                              jlong wfbngLinkN) {
    WfbngLink *link = native(wfbngLinkN);
    std::lock_guard<std::mutex> lock(link->sta_mutex);
    return static_cast<jint>(link->ap_recovery_base + (link->sta_link ? link->sta_link->recoveries() : 0));
}

// Sends one line to a TCP service on the camera (air_man_ap on 12355) over the AP
// link and returns its reply, or null. Blocks: call from a background thread.
extern "C" JNIEXPORT jstring JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeCameraCommand(
    JNIEnv *env,
    jclass clazz,
    jlong wfbngLinkN,
    jstring cameraIp,
    jint port,
    jstring command,
    jint timeoutMs,
    jboolean halfClose) {
    WfbngLink *link = native(wfbngLinkN);
    std::shared_ptr<StaLink> sta;
    {
        std::lock_guard<std::mutex> lock(link->sta_mutex);
        sta = link->sta_link;
    }
    if (!sta) return nullptr;

    const char *ip_c = env->GetStringUTFChars(cameraIp, nullptr);
    std::array<uint8_t, 4> ip{};
    const bool ip_ok = ip_c && inet_pton(AF_INET, ip_c, ip.data()) == 1;
    if (ip_c) env->ReleaseStringUTFChars(cameraIp, ip_c);
    const char *cmd_c = env->GetStringUTFChars(command, nullptr);
    std::string request = cmd_c ? cmd_c : "";
    if (cmd_c) env->ReleaseStringUTFChars(command, cmd_c);
    if (!ip_ok || port <= 0 || port > 65535 || request.empty()) return nullptr;
    if (request.back() != '\n') request += '\n';

    std::string reply;
    if (!sta->tcpRequest(
            ip, static_cast<uint16_t>(port), request, std::chrono::milliseconds(timeoutMs), halfClose, reply)) {
        return nullptr;
    }
    return env->NewStringUTF(printableAscii(reply).c_str());
}

// Modified start_link_quality_thread: use adaptive_link_enabled and adaptive_tx_power
void WfbngLink::applyTxPower(int fd, int power) {
    if (link_mode.load() == LinkMode::AP) {
        std::shared_ptr<StaLink> sta;
        {
            std::lock_guard<std::mutex> lock(sta_mutex);
            sta = sta_link;
        }
        if (sta) {
            sta->requestTxPower(power);
            return;
        }
    }
    if (auto dev = getDevice(fd)) {
        dev->SetTxPower(power);
    }
}

void WfbngLink::start_link_quality_thread(int fd) {
    if (link_mode.load() == LinkMode::AP) {
        // The adaptive link talks to the camera through the wfb-ng tunnel, which
        // does not exist in AP mode. Only the TX power applies.
        applyTxPower(fd, adaptive_tx_power);
        return;
    }
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

                // Use the new public FEC threshold values
                if (quality.lost_last_second > fec_lost_to_5) {
                    fec.bump(5); // Bump to FEC 5
                } else if (quality.recovered_last_second > fec_recovered_to_4) {
                    fec.bump(4); // Bump to FEC 4
                } else if (quality.recovered_last_second > fec_recovered_to_3) {
                    fec.bump(3); // Bump to FEC 3
                } else if (quality.recovered_last_second > fec_recovered_to_2) {
                    fec.bump(2); // Bump to FEC 2
                } else if (quality.recovered_last_second > fec_recovered_to_1) {
                    fec.bump(1); // Bump to FEC 1
                }

                snprintf(message + sizeof(len),
                         sizeof(message) - sizeof(len),
                         "%ld:%d:%d:%d:%d:%d:%f:0:-1:%d:%s\n",
                         static_cast<long>(currentEpoch),
                         quality.quality,
                         quality.quality,
                         quality.recovered_last_second,
                         quality.lost_last_second,
                         quality.quality,
                         quality.snr,
                         fec.value(),
                         quality.idr_code.c_str());
                len = strlen(message + sizeof(len));
                len = htonl(len);
                memcpy(message, &len, sizeof(len));
                __android_log_print(ANDROID_LOG_ERROR, TAG, " message %s", message + 4);
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
    applyTxPower(fd, adaptive_tx_power);
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
    if (const int fd = link->current_fd.load(); fd != -1) {
        link->applyTxPower(fd, power);
    }
    // If adaptive mode is enabled and the adaptive thread is not running, restart it.
    if (link->adaptive_link_enabled) {
        link->stop_adaptive_link();
        if (link->current_fd != -1) {
            link->start_link_quality_thread(link->current_fd);
        }
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseFec(JNIEnv *env,
                                                                                          jclass clazz,
                                                                                          jlong wfbngLinkN,
                                                                                          jint use) {
    WfbngLink *link = native(wfbngLinkN);
    link->fec.setEnabled(use);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseLdpc(JNIEnv *env,
                                                                                           jclass clazz,
                                                                                           jlong wfbngLinkN,
                                                                                           jint use) {
    WfbngLink *link = native(wfbngLinkN);
    link->ldpc_enabled = (use != 0);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseStbc(JNIEnv *env,
                                                                                           jclass clazz,
                                                                                           jlong wfbngLinkN,
                                                                                           jint use) {
    WfbngLink *link = native(wfbngLinkN);
    link->stbc_enabled = (use != 0);
}

extern "C" JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetFecThresholds(
    JNIEnv *env, jclass clazz, jlong nativeInstance, jint lostTo5, jint recTo4, jint recTo3, jint recTo2, jint recTo1) {
    WfbngLink *link = reinterpret_cast<WfbngLink *>(nativeInstance);
    if (!link) return;
    link->fec_lost_to_5 = lostTo5;
    link->fec_recovered_to_4 = recTo4;
    link->fec_recovered_to_3 = recTo3;
    link->fec_recovered_to_2 = recTo2;
    link->fec_recovered_to_1 = recTo1;
}