
#ifndef FPVUE_ANDROIDMEDIAFORMATHELPER_H
#define FPVUE_ANDROIDMEDIAFORMATHELPER_H

#include <media/NdkMediaFormat.h>
#include "../NALU/KeyFrameFinder.hpp"

// Some of these params are only supported on the latest Android versions
// However,writing them has no negative affect on devices with older Android versions
// Note that for example the low-latency key cannot fix any issues like the 'VUI' issue
void writeAndroidPerformanceParams(AMediaFormat* format)
{
    // I think: KEY_LOW_LATENCY is for decoder. But it doesn't really make a difference anyways
    static const auto PARAMETER_KEY_LOW_LATENCY = "low-latency";
    AMediaFormat_setInt32(format, PARAMETER_KEY_LOW_LATENCY, 1);
    // Lower values mean higher priority
    // Works on pixel 3 (look at output format description)
    static const auto AMEDIAFORMAT_KEY_PRIORITY = "priority";
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PRIORITY, 0);
    // set operating rate ? - doesn't make a difference
    // static const auto AMEDIAFORMAT_KEY_OPERATING_RATE="operating-rate";
    // AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_OPERATING_RATE,60);
    //
    // AMEDIAFORMAT_KEY_LOW_LATENCY;
    // AMEDIAFORMAT_KEY_LATENCY;
    // AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_LATENCY,0);
    // AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_OPERATING_RATE,0);
}

static void h264_configureAMediaFormat(KeyFrameFinder& kff, AMediaFormat* format)
{
    const auto sps     = kff.getCSD0();
    const auto pps     = kff.getCSD1();
    const auto videoWH = sps.getVideoWidthHeightSPS();
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, videoWH[0]);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, videoWH[1]);
    AMediaFormat_setBuffer(format, "csd-0", sps.getData(), (size_t) sps.getSize());
    AMediaFormat_setBuffer(format, "csd-1", pps.getData(), (size_t) pps.getSize());
    MLOGD << "Video WH:" << videoWH[0] << " H:" << videoWH[1];
    // AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_BIT_RATE,5*1024*1024);
    // AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_FRAME_RATE,60);
    // AVCProfileBaseline==1
    // AMediaFormat_setInt32(decoder.format,AMEDIAFORMAT_KEY_PROFILE,1);
    // AMediaFormat_setInt32(decoder.format,AMEDIAFORMAT_KEY_PRIORITY,0);
    // writeAndroidPerformanceParams(format);
}

static void h265_configureAMediaFormat(KeyFrameFinder& kff, AMediaFormat* format)
{
    std::vector<uint8_t> buff = {};
    const auto           sps  = kff.getCSD0();
    const auto           pps  = kff.getCSD1();
    const auto           vps  = kff.getVPS();
    buff.reserve(sps.getSize() + pps.getSize() + vps.getSize());
    KeyFrameFinder::appendNaluData(buff, vps);
    KeyFrameFinder::appendNaluData(buff, sps);
    KeyFrameFinder::appendNaluData(buff, pps);
    const auto videoWH = sps.getVideoWidthHeightSPS();
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, videoWH[0]);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, videoWH[1]);
    AMediaFormat_setBuffer(format, "csd-0", buff.data(), buff.size());
    MLOGD << "Video WH:" << videoWH[0] << " H:" << videoWH[1];
    // writeAndroidPerformanceParams(format);
}

#endif  // FPVUE_ANDROIDMEDIAFORMATHELPER_H
