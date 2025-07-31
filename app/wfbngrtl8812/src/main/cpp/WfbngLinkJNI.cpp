#include "WfbngLinkJNI.hpp"
#include "SignalQualityCalculator.h"
#include "AggregatorManager.hpp"
#include "DeviceManager.hpp"

#include <android/log.h>
#include <stdexcept>

#undef TAG
#define TAG "WfbngLinkJNI"

// Helper function implementations
WfbngLink* WfbngLinkJNI::getNativeInstance(jlong ptr) {
    return reinterpret_cast<WfbngLink*>(ptr);
}

jlong WfbngLinkJNI::getJniPointer(WfbngLink* instance) {
    return reinterpret_cast<intptr_t>(instance);
}

bool WfbngLinkJNI::validateInstance(WfbngLink* instance, const char* operation) {
    if (!instance) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid native instance in %s", operation);
        return false;
    }
    return true;
}

bool WfbngLinkJNI::handleJniException(JNIEnv* env, const char* operation) {
    if (env->ExceptionCheck()) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "JNI exception in %s", operation);
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

WfbConfiguration WfbngLinkJNI::createConfiguration(JNIEnv* env, jobject context) {
    // Create default configuration for JNI interface
    return WfbConfiguration::createDefault();
}

std::list<int> WfbngLinkJNI::javaListToStdList(JNIEnv* env, jobject list) {
    std::list<int> result;

    if (!list) {
        return result;
    }

    // Get the class and method IDs for java.util.List and its methods
    jclass listClass = env->GetObjectClass(list);
    jmethodID sizeMethod = env->GetMethodID(listClass, "size", "()I");
    jmethodID getMethod = env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;");

    // Method ID to get int value from Integer object
    jclass integerClass = env->FindClass("java/lang/Integer");
    jmethodID intValueMethod = env->GetMethodID(integerClass, "intValue", "()I");

    // Get the size of the list
    jint size = env->CallIntMethod(list, sizeMethod);

    // Iterate over the list and add elements to the C++ list
    for (int i = 0; i < size; ++i) {
        jobject element = env->CallObjectMethod(list, getMethod, i);
        jint value = env->CallIntMethod(element, intValueMethod);
        result.push_back(value);
    }

    return result;
}

// JNI method implementations

jlong WfbngLinkJNI::nativeInitialize(JNIEnv* env, jclass clazz, jobject context) {
    try {
        WfbConfiguration config = createConfiguration(env, context);
        auto* instance = new WfbngLink(env, context, config);
        return getJniPointer(instance);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to initialize WfbngLink: %s", e.what());
        return 0;
    }
}

void WfbngLinkJNI::nativeRun(JNIEnv* env, jclass clazz, jlong instance,
                            jobject context, jint wifiChannel, jint bandwidth, jint fd) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeRun")) {
        return;
    }

    try {
        link->run(env, context, wifiChannel, bandwidth, fd);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeRun: %s", e.what());
    }
}

void WfbngLinkJNI::nativeStartAdaptivelink(JNIEnv* env, jclass clazz, jlong instance) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeStartAdaptivelink")) {
        return;
    }

    if (!link->aggregator_manager || !link->aggregator_manager->getVideoAggregator()) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Cannot start adaptive link - aggregator not available");
        return;
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Manual adaptive link start requested");

    // Force enable and start adaptive link
    link->setAdaptiveLinkEnabled(true);

    if (link->current_fd != -1) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Starting adaptive link for fd=%d", link->current_fd);
    } else {
        __android_log_print(ANDROID_LOG_WARN, TAG, "No device fd available for adaptive link");
    }
}

jint WfbngLinkJNI::nativeGetSignalQuality(JNIEnv* env, jclass clazz) {
    try {
        return SignalQualityCalculator::get_instance().calculate_signal_quality().quality;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error getting signal quality: %s", e.what());
        return 0;
    }
}

void WfbngLinkJNI::nativeStop(JNIEnv* env, jclass clazz, jlong instance,
                             jobject context, jint fd) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeStop")) {
        return;
    }

    try {
        link->stop(env, context, fd);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeStop: %s", e.what());
    }
}

void WfbngLinkJNI::nativeCallBack(JNIEnv* env, jclass clazz,
                                 jobject callback, jlong instance) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeCallBack")) {
        return;
    }

    if (!link->aggregator_manager || !link->aggregator_manager->getVideoAggregator()) {
        return;
    }

    try {
        auto aggregator = link->aggregator_manager->getVideoAggregator();
        jclass jClassExtendsIWfbStatChangedI = env->GetObjectClass(callback);
        jclass jcStats = env->FindClass("com/openipc/wfbngrtl8812/WfbNGStats");

        if (jcStats == nullptr) {
            handleJniException(env, "nativeCallBack - FindClass");
            return;
        }

        jmethodID jcStatsConstructor = env->GetMethodID(jcStats, "<init>", "(IIIIIIII)V");
        if (jcStatsConstructor == nullptr) {
            handleJniException(env, "nativeCallBack - GetMethodID constructor");
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
            handleJniException(env, "nativeCallBack - NewObject");
            return;
        }

        jmethodID onStatsChanged = env->GetMethodID(
            jClassExtendsIWfbStatChangedI, "onWfbNgStatsChanged", "(Lcom/openipc/wfbngrtl8812/WfbNGStats;)V");

        if (onStatsChanged == nullptr) {
            handleJniException(env, "nativeCallBack - GetMethodID onStatsChanged");
            return;
        }

        env->CallVoidMethod(callback, onStatsChanged, stats);
        link->should_clear_stats = true;

    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeCallBack: %s", e.what());
    }
}

void WfbngLinkJNI::nativeRefreshKey(JNIEnv* env, jclass clazz, jlong instance) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeRefreshKey")) {
        return;
    }

    try {
        link->initAgg();
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeRefreshKey: %s", e.what());
    }
}

void WfbngLinkJNI::nativeSetAdaptiveLinkEnabled(JNIEnv* env, jclass clazz,
                                               jlong instance, jboolean enabled) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeSetAdaptiveLinkEnabled")) {
        return;
    }

    try {
        link->setAdaptiveLinkEnabled(enabled);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeSetAdaptiveLinkEnabled: %s", e.what());
    }
}

void WfbngLinkJNI::nativeSetTxPower(JNIEnv* env, jclass clazz,
                                   jlong instance, jint power) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeSetTxPower")) {
        return;
    }

    try {
        link->adaptive_tx_power = power;

        if (link->current_fd != -1 && link->device_manager &&
            link->device_manager->hasDevice(link->current_fd)) {
            auto device = link->device_manager->getDevice(link->current_fd);
            if (device) {
                device->setTxPower(power);
            }
        }

        // Update TX power in adaptive controller
        link->setAdaptiveTxPower(power);

    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeSetTxPower: %s", e.what());
    }
}

void WfbngLinkJNI::nativeSetUseFec(JNIEnv* env, jclass clazz,
                                  jlong instance, jint enabled) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeSetUseFec")) {
        return;
    }

    try {
        link->fec.setEnabled(enabled);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeSetUseFec: %s", e.what());
    }
}

void WfbngLinkJNI::nativeSetUseLdpc(JNIEnv* env, jclass clazz,
                                   jlong instance, jint enabled) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeSetUseLdpc")) {
        return;
    }

    try {
        link->ldpc_enabled = (enabled != 0);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeSetUseLdpc: %s", e.what());
    }
}

void WfbngLinkJNI::nativeSetUseStbc(JNIEnv* env, jclass clazz,
                                   jlong instance, jint enabled) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeSetUseStbc")) {
        return;
    }

    try {
        link->stbc_enabled = (enabled != 0);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeSetUseStbc: %s", e.what());
    }
}

void WfbngLinkJNI::nativeSetFecThresholds(JNIEnv* env, jclass clazz, jlong instance,
                                         jint lostTo5, jint recTo4, jint recTo3,
                                         jint recTo2, jint recTo1) {
    WfbngLink* link = getNativeInstance(instance);
    if (!validateInstance(link, "nativeSetFecThresholds")) {
        return;
    }

    try {
        link->setFecThresholds(lostTo5, recTo4, recTo3, recTo2, recTo1);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Error in nativeSetFecThresholds: %s", e.what());
    }
}

// C-style JNI function implementations that delegate to the class methods
extern "C" {

JNIEXPORT jlong JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeInitialize(
    JNIEnv* env, jclass clazz, jobject context) {
    return WfbngLinkJNI::nativeInitialize(env, clazz, context);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRun(
    JNIEnv* env, jclass clazz, jlong instance, jobject context,
    jint wifiChannel, jint bandwidth, jint fd) {
    WfbngLinkJNI::nativeRun(env, clazz, instance, context, wifiChannel, bandwidth, fd);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStartAdaptivelink(
    JNIEnv* env, jclass clazz, jlong instance) {
    WfbngLinkJNI::nativeStartAdaptivelink(env, clazz, instance);
}

JNIEXPORT jint JNICALL Java_com_openipc_pixelpilot_UsbSerialService_nativeGetSignalQuality(
    JNIEnv* env, jclass clazz) {
    return WfbngLinkJNI::nativeGetSignalQuality(env, clazz);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStop(
    JNIEnv* env, jclass clazz, jlong instance, jobject context, jint fd) {
    WfbngLinkJNI::nativeStop(env, clazz, instance, context, fd);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeCallBack(
    JNIEnv* env, jclass clazz, jobject callback, jlong instance) {
    WfbngLinkJNI::nativeCallBack(env, clazz, callback, instance);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRefreshKey(
    JNIEnv* env, jclass clazz, jlong instance) {
    WfbngLinkJNI::nativeRefreshKey(env, clazz, instance);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetAdaptiveLinkEnabled(
    JNIEnv* env, jclass clazz, jlong instance, jboolean enabled) {
    WfbngLinkJNI::nativeSetAdaptiveLinkEnabled(env, clazz, instance, enabled);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetTxPower(
    JNIEnv* env, jclass clazz, jlong instance, jint power) {
    WfbngLinkJNI::nativeSetTxPower(env, clazz, instance, power);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseFec(
    JNIEnv* env, jclass clazz, jlong instance, jint enabled) {
    WfbngLinkJNI::nativeSetUseFec(env, clazz, instance, enabled);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseLdpc(
    JNIEnv* env, jclass clazz, jlong instance, jint enabled) {
    WfbngLinkJNI::nativeSetUseLdpc(env, clazz, instance, enabled);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseStbc(
    JNIEnv* env, jclass clazz, jlong instance, jint enabled) {
    WfbngLinkJNI::nativeSetUseStbc(env, clazz, instance, enabled);
}

JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetFecThresholds(
    JNIEnv* env, jclass clazz, jlong instance, jint lostTo5, jint recTo4,
    jint recTo3, jint recTo2, jint recTo1) {
    WfbngLinkJNI::nativeSetFecThresholds(env, clazz, instance, lostTo5, recTo4, recTo3, recTo2, recTo1);
}

} // extern "C"