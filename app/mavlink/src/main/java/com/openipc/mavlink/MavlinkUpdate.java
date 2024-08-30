package com.openipc.mavlink;

public interface MavlinkUpdate {
    void onNewMavlinkData(final MavlinkData data);
}