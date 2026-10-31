//
// Created by gaeta on 2024-04-01.
//

#ifndef FPVUE_VIDEODECODER_H
#define FPVUE_VIDEODECODER_H

#include <android/log.h>
#include <android/native_window.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include "NALU/KeyFrameFinder.hpp"
#include "NALU/NALU.hpp"
#include "helper/TimeHelper.hpp"

struct DecodingInfo
{
    std::chrono::steady_clock::time_point lastCalculation          = std::chrono::steady_clock::now();
    long                                  nNALU                    = 0;
    long                                  nNALUSFeeded             = 0;
    long                                  nDecodedFrames           = 0;
    long                                  nCodec                   = 0;
    float                                 currentFPS               = 0;
    float                                 currentKiloBitsPerSecond = 0;
    float                                 avgParsingTime_ms        = 0;
    float                                 avgWaitForInputBTime_ms  = 0;
    float                                 avgDecodingTime_ms       = 0;

    bool operator==(const DecodingInfo& d2) const
    {
        return nNALU == d2.nNALU && nNALUSFeeded == d2.nNALUSFeeded && currentFPS == d2.currentFPS &&
               currentKiloBitsPerSecond == d2.currentKiloBitsPerSecond && avgParsingTime_ms == d2.avgParsingTime_ms &&
               avgWaitForInputBTime_ms == d2.avgWaitForInputBTime_ms && avgDecodingTime_ms == d2.avgDecodingTime_ms;
    }

    bool operator!=(const DecodingInfo& d2) const { return !(*this == d2); }
};

struct VideoRatio
{
    int width  = 0;
    int height = 0;

    bool operator==(const VideoRatio& b) const { return width == b.width && height == b.height; }

    bool operator!=(const VideoRatio& b) const { return !(*this == b); }
};

// Handles decoding of .h264 and .h265 video
// with low latency. Uses the AMediaCodec api
class VideoDecoder
{
  private:
    struct Decoder
    {
        bool           configured[2] = {false, false};
        AMediaCodec*   codec[2]      = {nullptr, nullptr};
        ANativeWindow* window[2]     = {nullptr, nullptr};
    };

  public:
    // Make sure to do no heavy lifting on this callback, since it is called from the low-latency mCheckOutputThread
    // thread (best to copy values and leave processing to another thread) The decoding info callback is called every
    // DECODING_INFO_RECALCULATION_INTERVAL_MS
    typedef std::function<void(const DecodingInfo)> DECODING_INFO_CHANGED_CALLBACK;
    // The decoder ratio callback is called every time the output format changes
    typedef std::function<void(const VideoRatio)> DECODER_RATIO_CHANGED;

  public:
    // We cannot initialize the Decoder until we have SPS and PPS data -
    // when streaming this data will be available at some point in future
    // Therefore we don't allocate the MediaCodec resources here
    VideoDecoder(JNIEnv* env);

    // This call acquires or releases the output surface
    // After acquiring the surface, the decoder will be started as soon as enough configuration data was passed to it
    // When releasing the surface, the decoder will be stopped if running and any resources will be freed
    // After releasing the surface it is safe for the android os to delete it
    void setOutputSurface(JNIEnv* env, jobject surface, jint idx);

    // register the specified callbacks. Only one can be registered at a time
    void registerOnDecoderRatioChangedCallback(DECODER_RATIO_CHANGED decoderRatioChangedC);

    void registerOnDecodingInfoChangedCallback(DECODING_INFO_CHANGED_CALLBACK decodingInfoChangedCallback);

    // Enable / disable the low-latency and realtime-priority AMediaFormat keys.
    // Applied the next time the decoder is configured, not to a running decoder.
    void setLowLatency(bool enabled) { mLowLatency = enabled; }

    // If the decoder has been configured, feed NALU. Else search for configuration data and
    // configure as soon as possible
    //  If the input pipe was closed (surface has been removed or is not set yet), only buffer key frames
    void interpretNALU(const NALU& nalu);

    // End of an access unit, from the RTP marker bit: queue the NALs gathered
    // since the last one as one input buffer. Some decoders show only the
    // first slice of a multi-slice frame fed one NAL per buffer.
    void endAccessUnit();

  private:
    // Initialize decoder with SPS / PPS data from KeyFrameFinder
    // Set Decoder.configured to true on success
    void configureStartDecoder(int idx);

    // Queue the gathered access unit as one input buffer (both decoder
    // instances) and clear it. Caller holds mMutexInputPipe.
    void flushAccessUnitLocked();
    void feedAccessUnit(const uint8_t* data, size_t size, bool configOnly, int idx);

    // Runs until EOS arrives at output buffer or decoder is stopped
    void checkOutputLoop(int idx);

    // Debug log
    void printAvgLog();

    void resetStatistics();

    std::unique_ptr<std::thread> mCheckOutputThread[2]  = {nullptr, nullptr};
    bool                         USE_SW_DECODER_INSTEAD = false;
    // Some decoders misbehave with the low-latency keys, so it stays user switchable.
    std::atomic<bool> mLowLatency = true;
    // Holds the AMediaCodec instance, as well as the state (configured or not configured)
    Decoder      decoder{};
    DecodingInfo decodingInfo;
    // The input pipe is closed until we set a valid surface
    bool       inputPipeClosed = true;
    std::mutex mMutexInputPipe;
    // Access-unit accumulator (endAccessUnit): the NALs of the current AU,
    // flushed on the transport's AU end or on the next first-slice NAL.
    std::vector<uint8_t>                  mAuBuffer;
    static constexpr size_t               kMaxAccessUnitBytes = 4 * 1024 * 1024;
    bool                                  mAuHasVcl           = false;
    std::chrono::steady_clock::time_point mAuFirstNaluTime;
    DECODER_RATIO_CHANGED                 onDecoderRatioChangedCallback = nullptr;
    DECODING_INFO_CHANGED_CALLBACK        onDecodingInfoChangedCallback = nullptr;
    // So we can temporarily attach the output thread to the vm and make ndk calls
    JavaVM*                               javaVm  = nullptr;
    std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
    RelativeCalculator                    nDecodedFrames;
    RelativeCalculator                    nNALUBytesFed;
    AvgCalculator                         parsingTime;
    AvgCalculator                         waitForInputB;
    AvgCalculator                         decodingTime;
    // Every n ms re-calculate the Decoding info
    static const constexpr auto DECODING_INFO_RECALCULATION_INTERVAL = std::chrono::milliseconds(1000);
    static constexpr const bool PRINT_DEBUG_INFO                     = true;
    static constexpr auto       TIME_BETWEEN_LOGS                    = std::chrono::seconds(5);
    static constexpr int64_t    BUFFER_TIMEOUT_US = 17 * 1000;  // 17ms (a little bit more than 17 ms (==60 fps))
  private:
    KeyFrameFinder mKeyFrameFinder;
    bool           IS_H265 = false;
};

#endif  // FPVUE_VIDEODECODER_H
