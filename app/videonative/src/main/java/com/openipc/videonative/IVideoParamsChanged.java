package com.openipc.videonative;

import androidx.annotation.Keep;

@Keep
public interface IVideoParamsChanged {
    void onVideoRatioChanged(int videoW, int videoH);

    void onDecodingInfoChanged(final DecodingInfo decodingInfo);
}