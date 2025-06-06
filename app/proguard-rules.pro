-keep class com.openipc.mavlink.MavlinkData { *; }
-keep interface com.openipc.mavlink.MavlinkUpdate { *; }
-keep class * implements com.openipc.mavlink.MavlinkUpdate { *; }
-keep class com.openipc.mavlink.MavlinkNative { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
