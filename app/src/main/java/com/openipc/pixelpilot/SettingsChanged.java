package com.openipc.pixelpilot;

public interface SettingsChanged {
    void onChannelSettingChanged(final int channel);
    void onBandwidthSettingChanged(final int bw);
}
