package com.geehe.videonative;

// Also called by native code
public interface IVideoParamsChanged {
    void onVideoRatioChanged(int videoW, int videoH);

    void onDecodingInfoChanged(final DecodingInfo decodingInfo);
}