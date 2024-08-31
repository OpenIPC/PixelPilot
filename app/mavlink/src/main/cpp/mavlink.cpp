#include <jni.h>
#include <string>

#include <sys/prctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <sys/prctl.h>
#include <sys/sem.h>
#include <thread>
#include <assert.h>
#include <android/log.h>

#include "mavlink/common/mavlink.h"
#include "mavlink.h"

#define TAG "pixelpilot"

long distance_meters_between(double lat1, double lon1, double lat2, double lon2) {
    double delta = (lon1 - lon2) * 0.017453292519;
    double sdlong = sin(delta);
    double cdlong = cos(delta);

    lat1 = (lat1) * 0.017453292519;
    lat2 = (lat2) * 0.017453292519;

    double slat1 = sin(lat1);
    double clat1 = cos(lat1);
    double slat2 = sin(lat2);
    double clat2 = cos(lat2);

    delta = (clat1 * slat2) - (slat1 * clat2 * cdlong);
    delta = delta * delta;
    delta += (clat2 * sdlong) * (clat2 * sdlong);
    delta = sqrt(delta);

    float denom = (slat1 * slat2) + (clat1 * clat2 * cdlong);
    delta = atan2(delta, denom);

    return (delta * 6372795.0);
}

int mavlink_thread_signal = 0;
std::atomic<bool> latestMavlinkDataChange = false;

void *listen(int mavlink_port) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Starting mavlink thread...");
    // Create socket
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG,
                            "ERROR: Unable to create MavLink socket:  %s", strerror(errno));
        return 0;
    }

    // Bind port
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Mavlink listening on %d", mavlink_port);
    struct sockaddr_in addr = {};
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "0.0.0.0", &(addr.sin_addr));
    addr.sin_port = htons(mavlink_port);

    if (bind(fd, (struct sockaddr *) (&addr), sizeof(addr)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Unable to bind MavLink port %d: %s",
                            mavlink_port, strerror(errno));
        return 0;
    }

    // Set Rx timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "Unable to bind MavLink rx timeout:  %s", strerror(errno));
        return 0;
    }

    char buffer[2048];
    while (!mavlink_thread_signal) {
        memset(buffer, 0x00, sizeof(buffer));
        int ret = recv(fd, buffer, sizeof(buffer), 0);
        if (ret < 0) {
            continue;
        } else if (ret == 0) {
            // peer has done an orderly shutdown
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Shutting down mavlink: ret=0");
            return 0;
        }

        // Parse
        mavlink_message_t msgMav;
        mavlink_status_t status;
        uint32_t tmp32;
        uint8_t tmp8;
        char szBuff[512];
        for (int i = 0; i < ret; ++i) {
            if (mavlink_parse_char(MAVLINK_COMM_0, buffer[i], &msgMav, &status) == 1) {
                switch (msgMav.msgid) {
                    case MAVLINK_MSG_ID_HEARTBEAT:
                        tmp32 = mavlink_msg_heartbeat_get_custom_mode(&msgMav);
                        tmp8 = mavlink_msg_heartbeat_get_base_mode(&msgMav);
                        latestMavlinkData.flight_mode = 0;
                        if (tmp8 & MAV_MODE_FLAG_SAFETY_ARMED) {
                            latestMavlinkData.telemetry_arm = 1;
                            if (latestMavlinkData.gps_fix_type != 0) {
                                latestMavlinkData.telemetry_lat_base = latestMavlinkData.telemetry_lat;
                                latestMavlinkData.telemetry_lon_base = latestMavlinkData.telemetry_lon;
                            } else {
                                latestMavlinkData.telemetry_lat_base = 0;
                                latestMavlinkData.telemetry_lon_base = 0;
                            }
                        } else {
                            latestMavlinkData.telemetry_arm = 0;
                        }

                        switch (tmp32) {
                            case PLANE_MODE_MANUAL:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_MANUAL;
                                break;
                            case PLANE_MODE_CIRCLE:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_CIRCLE;
                                break;
                            case PLANE_MODE_STABILIZE:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_STAB;
                                break;
                            case PLANE_MODE_FLY_BY_WIRE_A:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_FBWA;
                                break;
                            case PLANE_MODE_FLY_BY_WIRE_B:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_FBWB;
                                break;
                            case PLANE_MODE_ACRO:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_ACRO;
                                break;
                            case PLANE_MODE_AUTO:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_AUTO;
                                break;
                            case PLANE_MODE_AUTOTUNE:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_AUTOTUNE;
                                break;
                            case PLANE_MODE_RTL:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_RTL;
                                break;
                            case PLANE_MODE_LOITER:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_LOITER;
                                break;
                            case PLANE_MODE_TAKEOFF:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_TAKEOFF;
                                break;
                            case PLANE_MODE_CRUISE:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_CRUISE;
                                break;
                            case PLANE_MODE_QSTABILIZE:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_QSTAB;
                                break;
                            case PLANE_MODE_QHOVER:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_QHOVER;
                                break;
                            case PLANE_MODE_QLOITER:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_QLOITER;
                                break;
                            case PLANE_MODE_QLAND:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_QLAND;
                                break;
                            case PLANE_MODE_QRTL:
                                latestMavlinkData.flight_mode = FLIGHT_MODE_QRTL;
                                break;
                        }

                        break;

                    case MAVLINK_MSG_ID_STATUSTEXT:
                        mavlink_msg_statustext_get_text(&msgMav, szBuff);
                        memcpy(latestMavlinkData.status_text, szBuff, 101);
                        break;

                    case MAVLINK_MSG_ID_STATUSTEXT_LONG:
                        mavlink_msg_statustext_long_get_text(&msgMav, szBuff);
                        memcpy(latestMavlinkData.status_text, szBuff, 101);
                        break;

                    case MAVLINK_MSG_ID_SYS_STATUS: {
                        mavlink_sys_status_t bat;
                        mavlink_msg_sys_status_decode(&msgMav, &bat);
                        latestMavlinkData.telemetry_battery = bat.voltage_battery;
                        latestMavlinkData.telemetry_current = bat.current_battery;
                        latestMavlinkDataChange = true;
                    }
                        break;

                    case MAVLINK_MSG_ID_BATTERY_STATUS: {
                        mavlink_battery_status_t batt;
                        mavlink_msg_battery_status_decode(&msgMav, &batt);
                        latestMavlinkData.telemetry_current_consumed = batt.current_consumed;
                        latestMavlinkDataChange = true;
                    }
                        break;

                    case MAVLINK_MSG_ID_RC_CHANNELS_RAW:
                    case MAVLINK_MSG_ID_RC_CHANNELS: {
                        int tmpi = (int) ((uint8_t) mavlink_msg_rc_channels_raw_get_rssi(&msgMav));
                        latestMavlinkData.telemetry_rssi = (tmpi * 100) / 255;
                        latestMavlinkData.telemetry_resolution = mavlink_msg_rc_channels_raw_get_chan8_raw(
                                &msgMav);
                        latestMavlinkDataChange = true;
                    }
                        break;

                    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
                        latestMavlinkData.heading =
                                mavlink_msg_global_position_int_get_hdg(&msgMav) / 100.0f;
                        latestMavlinkData.telemetry_altitude =
                                mavlink_msg_global_position_int_get_relative_alt(&msgMav) / 10.0f +
                                100000;
                        latestMavlinkData.telemetry_lat = mavlink_msg_global_position_int_get_lat(
                                &msgMav);
                        latestMavlinkData.telemetry_lon = mavlink_msg_global_position_int_get_lon(
                                &msgMav);
                        if (latestMavlinkData.gps_fix_type != 0 &&
                            latestMavlinkData.telemetry_arm == 1) {
                            latestMavlinkData.telemetry_distance = 100 * distance_meters_between(
                                    latestMavlinkData.telemetry_lat_base / 10000000.0,
                                    latestMavlinkData.telemetry_lon_base / 10000000.0,
                                    latestMavlinkData.telemetry_lat / 10000000.0,
                                    latestMavlinkData.telemetry_lon / 10000000.0);
                        } else {
                            latestMavlinkData.telemetry_distance = 0;
                        }
                        latestMavlinkDataChange = true;
                        break;
                    }

                    case MAVLINK_MSG_ID_GPS_RAW_INT: {
                        latestMavlinkData.gps_fix_type = mavlink_msg_gps_raw_int_get_fix_type(
                                &msgMav);
                        latestMavlinkData.telemetry_sats = mavlink_msg_gps_raw_int_get_satellites_visible(
                                &msgMav);
                        latestMavlinkData.hdop = mavlink_msg_gps_raw_int_get_eph(&msgMav);
                        latestMavlinkData.telemetry_lat = mavlink_msg_gps_raw_int_get_lat(&msgMav);
                        latestMavlinkData.telemetry_lon = mavlink_msg_gps_raw_int_get_lon(&msgMav);
                        latestMavlinkDataChange = true;
                    }
                        break;

                    case MAVLINK_MSG_ID_VFR_HUD: {
                        latestMavlinkData.telemetry_throttle = mavlink_msg_vfr_hud_get_throttle(
                                &msgMav);
                        latestMavlinkData.telemetry_vspeed =
                                mavlink_msg_vfr_hud_get_climb(&msgMav) * 100 + 100000;
                        latestMavlinkData.telemetry_gspeed =
                                mavlink_msg_vfr_hud_get_groundspeed(&msgMav) * 100.0f + 100000;
                        latestMavlinkDataChange = true;
                    }
                        break;

                    case MAVLINK_MSG_ID_ATTITUDE: {
                        mavlink_attitude_t att;
                        mavlink_msg_attitude_decode(&msgMav, &att);
                        latestMavlinkData.telemetry_pitch =
                                att.pitch * (180.0 / 3.141592653589793238463);
                        latestMavlinkData.telemetry_roll =
                                att.roll * (180.0 / 3.141592653589793238463);
                        latestMavlinkData.telemetry_yaw =
                                att.yaw * (180.0 / 3.141592653589793238463);
                        latestMavlinkDataChange = true;
                    }
                        break;

                    case MAVLINK_MSG_ID_RADIO_STATUS: {
                        if ((msgMav.sysid != 3) || (msgMav.compid != 68)) {
                            break;
                        }
                        latestMavlinkData.wfb_rssi = (int8_t) mavlink_msg_radio_status_get_rssi(
                                &msgMav);
                        latestMavlinkData.wfb_errors = mavlink_msg_radio_status_get_rxerrors(
                                &msgMav);
                        latestMavlinkData.wfb_fec_fixed = mavlink_msg_radio_status_get_fixed(
                                &msgMav);
                        latestMavlinkData.wfb_flags = mavlink_msg_radio_status_get_remnoise(
                                &msgMav);
                        latestMavlinkDataChange = true;
                    }
                        break;

                    default:
                        //printf("mavlink msg %d from %d/%d\n",
                               //msgMav.msgid, msgMav.sysid, msgMav.compid);
                        break;
                }
            }
        }
        usleep(1);
    }

    __android_log_print(ANDROID_LOG_DEBUG, TAG, "Mavlink thread done.");
    return 0;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_openipc_mavlink_MavlinkNative_nativeCallBack(JNIEnv *env, jclass clazz,
                                                      jobject mavlinkChangeI) {
//    g_context = mavlinkChangeI;
    //Update all java stuff
    if (latestMavlinkDataChange) {
        jclass jClassExtendsMavlinkChangeI = env->GetObjectClass(mavlinkChangeI);
        jclass jcMavlinkData = env->FindClass("com/openipc/mavlink/MavlinkData");
        assert(jcMavlinkData != nullptr);
        jmethodID jcMavlinkDataConstructor = env->GetMethodID(jcMavlinkData, "<init>",
                                                              "(FFFFFFFDDDDDDFFFFBBBBBBLjava/lang/String;)V");
        assert(jcMavlinkDataConstructor != nullptr);
        jstring pJstring = env->NewStringUTF(latestMavlinkData.status_text);
        auto mavlinkData = env->NewObject(jcMavlinkData, jcMavlinkDataConstructor,
                                          (jfloat) latestMavlinkData.telemetry_altitude,
                                          (jfloat) latestMavlinkData.telemetry_pitch,
                                          (jfloat) latestMavlinkData.telemetry_roll,
                                          (jfloat) latestMavlinkData.telemetry_yaw,
                                          (jfloat) latestMavlinkData.telemetry_battery,
                                          (jfloat) latestMavlinkData.telemetry_current,
                                          (jfloat) latestMavlinkData.telemetry_current_consumed,
                                          (jdouble) latestMavlinkData.telemetry_lat,
                                          (jdouble) latestMavlinkData.telemetry_lon,
                                          (jdouble) latestMavlinkData.telemetry_lat_base,
                                          (jdouble) latestMavlinkData.telemetry_lon_base,
                                          (jdouble) latestMavlinkData.telemetry_hdg,
                                          (jdouble) latestMavlinkData.telemetry_distance,
                                          (jfloat) latestMavlinkData.telemetry_sats,
                                          (jfloat) latestMavlinkData.telemetry_gspeed,
                                          (jfloat) latestMavlinkData.telemetry_vspeed,
                                          (jfloat) latestMavlinkData.telemetry_throttle,
                                          (jbyte) latestMavlinkData.telemetry_arm,
                                          (jbyte) latestMavlinkData.flight_mode,
                                          (jbyte) latestMavlinkData.gps_fix_type,
                                          (jbyte) latestMavlinkData.hdop,
                                          (jbyte) latestMavlinkData.telemetry_rssi,
                                          (jbyte) latestMavlinkData.heading,
                                          pJstring);
        assert(mavlinkData != nullptr);
        jmethodID onNewMavlinkDataJAVA = env->GetMethodID(jClassExtendsMavlinkChangeI,
                                                          "onNewMavlinkData",
                                                          "(Lcom/openipc/mavlink/MavlinkData;)V");
        assert(onNewMavlinkDataJAVA != nullptr);
        env->CallVoidMethod(mavlinkChangeI, onNewMavlinkDataJAVA, mavlinkData);
        latestMavlinkDataChange = false;

        // Clean up local references
        env->DeleteLocalRef(jcMavlinkData);
        env->DeleteLocalRef(mavlinkData);
        env->DeleteLocalRef(pJstring);
    }
}
extern "C"
JNIEXPORT void JNICALL
Java_com_openipc_mavlink_MavlinkNative_nativeStart(JNIEnv *env, jclass clazz, jobject context) {
    auto threadFunction = []() {
        listen(14550);
    };
    std::thread mavlink_thread(threadFunction);
    mavlink_thread.detach();
}
extern "C"
JNIEXPORT void JNICALL
Java_com_openipc_mavlink_MavlinkNative_nativeStop(JNIEnv *env, jclass clazz, jobject context) {
    mavlink_thread_signal++;
}
