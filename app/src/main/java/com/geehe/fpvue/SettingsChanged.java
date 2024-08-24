package com.geehe.fpvue;

public interface SettingsChanged {
    void onChannelSettingChanged(final int channel);

    void onCodecSettingChanged(final String codec);
}
