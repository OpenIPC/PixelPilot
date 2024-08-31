package com.openipc.mavlink;

public class MavlinkData {
    public final float telemetryAltitude;
    public final float telemetryPitch;
    public final float telemetryRoll;
    public final float telemetryYaw;
    public final float telemetryBattery;
    public final float telemetryCurrent;
    public final float telemetryCurrentConsumed;
    public final double telemetryLat;
    public final double telemetryLon;
    public final double telemetryLatBase;
    public final double telemetryLonBase;
    public final double telemetryHdg;
    public final double telemetryDistance;
    public final float telemetrySat;
    public final float telemetryGSpeed;
    public final float telemetryVSpeed;
    public final float telemetryThrottle;
    public final byte telemetryArm;
    public final byte flight_mode;
    public final byte gps_fix_type;
    public final byte hdop;
    public final byte rssi;
    public final byte heading;
    public String status_text;

    public MavlinkData(float telemetryAltitude, float telemetryPitch, float telemetryRoll, float telemetryYaw,
                       float telemetryBattery, float telemetryCurrent, float telemetryCurrentConsumed,
                       double telemetryLat, double telemetryLon, double telemetryLatBase, double telemetryLonBase,
                       double telemetryHdg, double telemetryDistance, float telemetrySat, float telemetryGSpeed,
                       float telemetryVSpeed, float telemetryThrottle, byte telemetryArm, byte flight_mode,
                       byte gps_fix_type, byte hdop, byte rssi, byte heading, String status) {
        this.telemetryAltitude = telemetryAltitude;
        this.telemetryPitch = telemetryPitch;
        this.telemetryRoll = telemetryRoll;
        this.telemetryYaw = telemetryYaw;
        this.telemetryBattery = telemetryBattery;
        this.telemetryCurrent = telemetryCurrent;
        this.telemetryCurrentConsumed = telemetryCurrentConsumed;
        this.telemetryLat = telemetryLat;
        this.telemetryLon = telemetryLon;
        this.telemetryLatBase = telemetryLatBase;
        this.telemetryLonBase = telemetryLonBase;
        this.telemetryHdg = telemetryHdg;
        this.telemetryDistance = telemetryDistance;
        this.telemetrySat = telemetrySat;
        this.telemetryGSpeed = telemetryGSpeed;
        this.telemetryVSpeed = telemetryVSpeed;
        this.telemetryThrottle = telemetryThrottle;
        this.telemetryArm = telemetryArm;
        this.flight_mode = flight_mode;
        this.gps_fix_type = gps_fix_type;
        this.hdop = hdop;
        this.rssi = rssi;
        this.heading = heading;
        this.status_text = status;
    }
}