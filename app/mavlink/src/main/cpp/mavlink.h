//
// Created by gaeta on 2024-04-27.
//

#ifndef FPVUE_MAVLINK_H
#define FPVUE_MAVLINK_H

extern int mavlink_port;

size_t numOfChars(const char s[]);

char *insertString(char s1[], const char s2[], size_t pos);

struct mavlink_data {
    // Mavlink
    float telemetry_altitude;
    float telemetry_pitch;
    float telemetry_roll;
    float telemetry_yaw;
    float telemetry_battery;
    float telemetry_current;
    float telemetry_current_consumed;
    double telemetry_lat;
    double telemetry_lon;
    double telemetry_lat_base;
    double telemetry_lon_base;
    double telemetry_hdg;
    double telemetry_distance;
    float telemetry_sats;
    float telemetry_gspeed;
    float telemetry_vspeed;
    float telemetry_rssi;
    float telemetry_throttle;
    float telemetry_resolution;
    float telemetry_arm;
    char status_text[101];
    uint8_t flight_mode;
    uint8_t gps_fix_type;
    uint16_t hdop;
    uint16_t heading;
    int8_t wfb_rssi;
    uint16_t wfb_errors;
    uint16_t wfb_fec_fixed;
    int8_t wfb_flags;
} latestMavlinkData;

typedef enum PLANE_MODE {
    PLANE_MODE_MANUAL = 0, /*  | */
    PLANE_MODE_CIRCLE = 1, /*  | */
    PLANE_MODE_STABILIZE = 2, /*  | */
    PLANE_MODE_TRAINING = 3, /*  | */
    PLANE_MODE_ACRO = 4, /*  | */
    PLANE_MODE_FLY_BY_WIRE_A = 5, /*  | */
    PLANE_MODE_FLY_BY_WIRE_B = 6, /*  | */
    PLANE_MODE_CRUISE = 7, /*  | */
    PLANE_MODE_AUTOTUNE = 8, /*  | */
    PLANE_MODE_AUTO = 10, /*  | */
    PLANE_MODE_RTL = 11, /*  | */
    PLANE_MODE_LOITER = 12, /*  | */
    PLANE_MODE_TAKEOFF = 13, /*  | */
    PLANE_MODE_AVOID_ADSB = 14, /*  | */
    PLANE_MODE_GUIDED = 15, /*  | */
    PLANE_MODE_INITIALIZING = 16, /*  | */
    PLANE_MODE_QSTABILIZE = 17, /*  | */
    PLANE_MODE_QHOVER = 18, /*  | */
    PLANE_MODE_QLOITER = 19, /*  | */
    PLANE_MODE_QLAND = 20, /*  | */
    PLANE_MODE_QRTL = 21, /*  | */
    PLANE_MODE_QAUTOTUNE = 22, /*  | */
    PLANE_MODE_ENUM_END = 23, /*  | */
} PLANE_MODE;

#define FLIGHT_MODE_ARMED 128 // last bit set to 1, remaining 7 bits are a flight mode as int value
#define FLIGHT_MODE_MANUAL 1
#define FLIGHT_MODE_STAB 2
#define FLIGHT_MODE_ALTH 3
#define FLIGHT_MODE_LOITER 4
#define FLIGHT_MODE_RTL 5
#define FLIGHT_MODE_LAND 6
#define FLIGHT_MODE_POSHOLD 7
#define FLIGHT_MODE_AUTOTUNE 8
#define FLIGHT_MODE_ACRO 9
#define FLIGHT_MODE_FBWA 10
#define FLIGHT_MODE_FBWB 11
#define FLIGHT_MODE_CRUISE 12
#define FLIGHT_MODE_TAKEOFF 13
#define FLIGHT_MODE_RATE 15
#define FLIGHT_MODE_HORZ 16
#define FLIGHT_MODE_CIRCLE 17
#define FLIGHT_MODE_AUTO 18
#define FLIGHT_MODE_QSTAB 20
#define FLIGHT_MODE_QHOVER 21
#define FLIGHT_MODE_QLOITER 22
#define FLIGHT_MODE_QLAND 23
#define FLIGHT_MODE_QRTL 24

#define FC_TELE_FLAGS_ARMED 1
#define FC_TELE_FLAGS_POS_CURRENT 2 // received lon, lat are current position
#define FC_TELE_FLAGS_POS_HOME 4    // received lon, lat are home position
#define FC_TELE_FLAGS_HAS_GPS_FIX 8  // has a 3d gps fix
#define FC_TELE_FLAGS_RC_FAILSAFE 16   // RC failsafe is triggered
#define FC_TELE_FLAGS_NO_FC_TELEMETRY 32 // No telemetry from flight controller
#define FC_TELE_FLAGS_HAS_MESSAGE 64 // Set if there is also a message structure from flight controller;
#define FC_TELE_FLAGS_HAS_ATTITUDE 128 // Set if the attitude was received
#define FC_MESSAGE_MAX_LENGTH 101

typedef enum MAVLINK_GPS_FIX_TYPE {
    MAVLINK_GPS_FIX_TYPE_NO_GPS = 0, /* No GPS connected | */
    MAVLINK_GPS_FIX_TYPE_NO_FIX = 1, /* No position information, GPS is connected | */
    MAVLINK_GPS_FIX_TYPE_2D_FIX = 2, /* 2D position | */
    MAVLINK_GPS_FIX_TYPE_3D_FIX = 3, /* 3D position | */
    MAVLINK_GPS_FIX_TYPE_DGPS = 4, /* DGPS/SBAS aided 3D position | */
    MAVLINK_GPS_FIX_TYPE_RTK_FLOAT = 5, /* RTK float, 3D position | */
    MAVLINK_GPS_FIX_TYPE_RTK_FIXED = 6, /* RTK Fixed, 3D position | */
    MAVLINK_GPS_FIX_TYPE_STATIC = 7, /* Static fixed, typically used for base stations | */
    MAVLINK_GPS_FIX_TYPE_PPP = 8, /* PPP, 3D position. | */
    MAVLINK_GPS_FIX_TYPE_ENUM_END = 9, /*  | */
} MAVLINK_GPS_FIX_TYPE;

#endif //FPVUE_MAVLINK_H
