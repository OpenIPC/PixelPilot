#ifndef WFBNG_LINK_JNI_HPP
#define WFBNG_LINK_JNI_HPP

#include <jni.h>
#include <list>
#include "WfbngLink.hpp"
#include "WfbConfiguration.hpp"

/**
 * @brief JNI wrapper layer for WfbngLink
 *
 * This class provides a clean separation between the JNI interface and the core
 * WfbngLink functionality. It handles all JNI-specific concerns including:
 * - Type conversions between Java and C++
 * - Error handling and exception management
 * - Memory management for JNI objects
 * - Thread safety for JNI calls
 */
class WfbngLinkJNI {
public:
    /**
     * @brief Initialize a new WfbngLink instance
     * @param env JNI environment
     * @param clazz Java class reference
     * @param context Android context object
     * @return Pointer to native WfbngLink instance as jlong
     */
    static jlong nativeInitialize(JNIEnv* env, jclass clazz, jobject context);

    /**
     * @brief Start WFB link operation
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     * @param context Android context object
     * @param wifiChannel WiFi channel number
     * @param bandwidth Channel bandwidth
     * @param fd Device file descriptor
     */
    static void nativeRun(JNIEnv* env, jclass clazz, jlong instance,
                         jobject context, jint wifiChannel, jint bandwidth, jint fd);

    /**
     * @brief Start adaptive link functionality
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     */
    static void nativeStartAdaptivelink(JNIEnv* env, jclass clazz, jlong instance);

    /**
     * @brief Get current signal quality
     * @param env JNI environment
     * @param clazz Java class reference
     * @return Signal quality value
     */
    static jint nativeGetSignalQuality(JNIEnv* env, jclass clazz);

    /**
     * @brief Stop WFB link operation
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     * @param context Android context object
     * @param fd Device file descriptor
     */
    static void nativeStop(JNIEnv* env, jclass clazz, jlong instance,
                          jobject context, jint fd);

    /**
     * @brief Callback for statistics updates
     * @param env JNI environment
     * @param clazz Java class reference
     * @param callback Java callback object
     * @param instance Native WfbngLink instance pointer
     */
    static void nativeCallBack(JNIEnv* env, jclass clazz,
                              jobject callback, jlong instance);

    /**
     * @brief Refresh encryption keys
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     */
    static void nativeRefreshKey(JNIEnv* env, jclass clazz, jlong instance);

    /**
     * @brief Enable or disable adaptive link
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     * @param enabled Enable/disable flag
     */
    static void nativeSetAdaptiveLinkEnabled(JNIEnv* env, jclass clazz,
                                            jlong instance, jboolean enabled);

    /**
     * @brief Set transmission power
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     * @param power TX power value
     */
    static void nativeSetTxPower(JNIEnv* env, jclass clazz,
                                jlong instance, jint power);

    /**
     * @brief Enable or disable FEC
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     * @param enabled Enable/disable flag
     */
    static void nativeSetUseFec(JNIEnv* env, jclass clazz,
                               jlong instance, jint enabled);

    /**
     * @brief Enable or disable LDPC
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     * @param enabled Enable/disable flag
     */
    static void nativeSetUseLdpc(JNIEnv* env, jclass clazz,
                                jlong instance, jint enabled);

    /**
     * @brief Enable or disable STBC
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     * @param enabled Enable/disable flag
     */
    static void nativeSetUseStbc(JNIEnv* env, jclass clazz,
                                jlong instance, jint enabled);

    /**
     * @brief Set FEC threshold values
     * @param env JNI environment
     * @param clazz Java class reference
     * @param instance Native WfbngLink instance pointer
     * @param lostTo5 Lost packets threshold for FEC 5
     * @param recTo4 Recovered packets threshold for FEC 4
     * @param recTo3 Recovered packets threshold for FEC 3
     * @param recTo2 Recovered packets threshold for FEC 2
     * @param recTo1 Recovered packets threshold for FEC 1
     */
    static void nativeSetFecThresholds(JNIEnv* env, jclass clazz, jlong instance,
                                      jint lostTo5, jint recTo4, jint recTo3,
                                      jint recTo2, jint recTo1);

private:
    /**
     * @brief Convert jlong pointer to native WfbngLink instance
     * @param ptr JNI pointer as jlong
     * @return Pointer to WfbngLink instance
     */
    static WfbngLink* getNativeInstance(jlong ptr);

    /**
     * @brief Convert native WfbngLink pointer to jlong
     * @param instance Pointer to WfbngLink instance
     * @return JNI pointer as jlong
     */
    static jlong getJniPointer(WfbngLink* instance);

    /**
     * @brief Create configuration from Android context
     * @param env JNI environment
     * @param context Android context object
     * @return WfbConfiguration instance
     */
    static WfbConfiguration createConfiguration(JNIEnv* env, jobject context);

    /**
     * @brief Convert Java List<Integer> to std::list<int>
     * @param env JNI environment
     * @param javaList Java List object
     * @return C++ list of integers
     */
    static std::list<int> javaListToStdList(JNIEnv* env, jobject javaList);

    /**
     * @brief Handle JNI exceptions and log errors
     * @param env JNI environment
     * @param operation Name of the operation that failed
     * @return true if exception was handled, false otherwise
     */
    static bool handleJniException(JNIEnv* env, const char* operation);

    /**
     * @brief Validate native instance pointer
     * @param instance Pointer to validate
     * @param operation Name of the operation for error logging
     * @return true if valid, false otherwise
     */
    static bool validateInstance(WfbngLink* instance, const char* operation);
};

// C-style JNI function declarations for export
extern "C" {
    JNIEXPORT jlong JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeInitialize(
        JNIEnv* env, jclass clazz, jobject context);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRun(
        JNIEnv* env, jclass clazz, jlong instance, jobject context,
        jint wifiChannel, jint bandwidth, jint fd);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStartAdaptivelink(
        JNIEnv* env, jclass clazz, jlong instance);

    JNIEXPORT jint JNICALL Java_com_openipc_pixelpilot_UsbSerialService_nativeGetSignalQuality(
        JNIEnv* env, jclass clazz);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeStop(
        JNIEnv* env, jclass clazz, jlong instance, jobject context, jint fd);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeCallBack(
        JNIEnv* env, jclass clazz, jobject callback, jlong instance);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeRefreshKey(
        JNIEnv* env, jclass clazz, jlong instance);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetAdaptiveLinkEnabled(
        JNIEnv* env, jclass clazz, jlong instance, jboolean enabled);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetTxPower(
        JNIEnv* env, jclass clazz, jlong instance, jint power);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseFec(
        JNIEnv* env, jclass clazz, jlong instance, jint enabled);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseLdpc(
        JNIEnv* env, jclass clazz, jlong instance, jint enabled);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetUseStbc(
        JNIEnv* env, jclass clazz, jlong instance, jint enabled);

    JNIEXPORT void JNICALL Java_com_openipc_wfbngrtl8812_WfbNgLink_nativeSetFecThresholds(
        JNIEnv* env, jclass clazz, jlong instance, jint lostTo5, jint recTo4,
        jint recTo3, jint recTo2, jint recTo1);
}

#endif // WFBNG_LINK_JNI_HPP