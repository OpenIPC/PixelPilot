package com.geehe.mavlink;

public interface MavlinkUpdate {
    void onNewMavlinkData(final MavlinkData data);
}