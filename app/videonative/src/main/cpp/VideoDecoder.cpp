//
// Created by gaeta on 2024-04-01.
//

#include "VideoDecoder.h"
#include "AndroidThreadPrioValues.hpp"
#include "helper/NDKThreadHelper.hpp"
#include <unistd.h>
#include <sstream>
#include "helper/AndroidMediaFormatHelper.h"

#include <vector>

#include <android/native_window_jni.h>
#include <media/NdkMediaCodec.h>

using namespace std::chrono;

VideoDecoder::VideoDecoder(JNIEnv *env) {
    env->GetJavaVM(&javaVm);
    resetStatistics();
}

void VideoDecoder::setOutputSurface(JNIEnv *env, jobject surface) {
    if (surface == nullptr) {
        MLOGD << "Set output null surface";
        //assert(decoder.window!=nullptr);
        if (decoder.window == nullptr) {
            //MLOGD<<"Decoder window is already null";
            return;
        }
        std::lock_guard<std::mutex> lock(mMutexInputPipe);
        inputPipeClosed = true;
        if (decoder.configured) {
            AMediaCodec_stop(decoder.codec);
            AMediaCodec_delete(decoder.codec);
            mKeyFrameFinder.reset();
            decoder.configured = false;
            if (mCheckOutputThread->joinable()) {
                mCheckOutputThread->join();
                mCheckOutputThread.reset();
            }
        }
        ANativeWindow_release(decoder.window);
        decoder.window = nullptr;
        resetStatistics();
    } else {
        MLOGD << "Set output non-null surface";
        // Throw warning if the surface is set without clearing it first
        assert(decoder.window == nullptr);
        decoder.window = ANativeWindow_fromSurface(env, surface);
        // open the input pipe - now the decoder will start as soon as enough data is available
        inputPipeClosed = false;
    }
}

void
VideoDecoder::registerOnDecoderRatioChangedCallback(DECODER_RATIO_CHANGED decoderRatioChangedC) {
    onDecoderRatioChangedCallback = std::move(decoderRatioChangedC);
}

void VideoDecoder::registerOnDecodingInfoChangedCallback(
        DECODING_INFO_CHANGED_CALLBACK decodingInfoChangedCallback) {
    onDecodingInfoChangedCallback = std::move(decodingInfoChangedCallback);
}

void VideoDecoder::interpretNALU(const NALU &nalu) {
    // TODO: RN switching between h264 / h265 requires re-setting the surface
    if (decoder.configured) {
        assert(nalu.IS_H265_PACKET == IS_H265);
        //decoder.configured = nalu.IS_H265_PACKET==IS_H265;
    }
    IS_H265 = nalu.IS_H265_PACKET;
    //we need this lock, since the receiving/parsing/feeding does not run on the same thread who sets the input surface
    std::lock_guard<std::mutex> lock(mMutexInputPipe);
    decodingInfo.nNALU++;
    if (nalu.getSize() <= 4) {
        //No data in NALU (e.g at the beginning of a stream)
        return;
    }
    nNALUBytesFed.add(nalu.getSize());
    if (inputPipeClosed) {
        MLOGD << "inputPipeClosed.";
        //A feedD thread (e.g. file or udp) thread might be running even tough no output surface was set
        //But at least we can buffer the sps/pps data
        mKeyFrameFinder.saveIfKeyFrame(nalu);
        return;
    }
    if (decoder.configured) {
        feedDecoder(nalu);
        decodingInfo.nNALUSFeeded++;
        // manually feeding AUDs doesn't seem to change anything for high latency streams
        // Only for the x264 sw encoded example stream it might improve latency slightly
        //if(!nalu.IS_H265_PACKET && nalu.get_nal_unit_type()==NAL_UNIT_TYPE_CODED_SLICE_NON_IDR){
        //MLOGD<<"Feeding special AUD";
        //feedDecoder(NALU::createExampleH264_AUD());
        //}
    } else {
        // Store sps,pps, vps(H265 only)
        // As soon as enough data has been buffered to initialize the decoder,do so.
        mKeyFrameFinder.saveIfKeyFrame(nalu);
        if (mKeyFrameFinder.allKeyFramesAvailable(IS_H265)) {
            MLOGD << "Configuring decoder...";
            configureStartDecoder();
        }
    }
}

void VideoDecoder::configureStartDecoder() {
    const std::string MIME = IS_H265 ? "video/hevc" : "video/avc";
    decoder.codec = AMediaCodec_createDecoderByType(MIME.c_str());

    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, MIME.c_str());

    //AMediaFormat_setInt32(format, "low-latency", 1);
    //AMediaFormat_setInt32(format, "vendor.low-latency.enable", 1);
    //AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
    //AMediaFormat_setInt32(format, "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req", 1);
    //AMediaFormat_setInt32(format, "vendor.rtc-ext-dec-low-latency.enable", 1);

    // MediaCodec supports two priorities: 0 - realtime, 1 - best effort
    //AMediaFormat_setInt32(format, "priority", 0);

    if (IS_H265) {
        h265_configureAMediaFormat(mKeyFrameFinder, format);
    } else {
        h264_configureAMediaFormat(mKeyFrameFinder, format);
    }

    MLOGD << "Configuring decoder:" << AMediaFormat_toString(format);

    auto status = AMediaCodec_configure(decoder.codec, format, decoder.window, nullptr, 0);
    AMediaFormat_delete(format);

    switch (status) {
        case AMEDIA_OK: {
            MLOGD << "AMediaCodec_configure: OK";
            break;
        }
        case AMEDIA_ERROR_UNKNOWN: {
            MLOGD << "AMediaCodec_configure: AMEDIA_ERROR_UNKNOWN";
            break;
        }
        case AMEDIA_ERROR_MALFORMED: {
            MLOGD << "AMediaCodec_configure: AMEDIA_ERROR_MALFORMED";
            break;
        }
        case AMEDIA_ERROR_UNSUPPORTED: {
            MLOGD << "AMediaCodec_configure: AMEDIA_ERROR_UNSUPPORTED";
            break;
        }
        case AMEDIA_ERROR_INVALID_OBJECT: {
            MLOGD << "AMediaCodec_configure: AMEDIA_ERROR_INVALID_OBJECT";
            break;
        }
        case AMEDIA_ERROR_INVALID_PARAMETER: {
            MLOGD << "AMediaCodec_configure: AMEDIA_ERROR_INVALID_PARAMETER";
            break;
        }
        default: {
            break;
        }
    }


    if (decoder.codec == nullptr) {
        MLOGD << "Cannot configure decoder";
        //set csd-0 and csd-1 back to 0, maybe they were just faulty but we have better luck with the next ones
        //mKeyFrameFinder.reset();
        return;
    }
    AMediaCodec_start(decoder.codec);
    mCheckOutputThread = std::make_unique<std::thread>(&VideoDecoder::checkOutputLoop, this);
    NDKThreadHelper::setName(mCheckOutputThread->native_handle(), "LLDCheckOutput");
    decoder.configured = true;
}

void VideoDecoder::feedDecoder(const NALU &nalu) {
    const auto now = std::chrono::steady_clock::now();
    const auto deltaParsing = now - nalu.creationTime;
    while (true) {
        const auto index = AMediaCodec_dequeueInputBuffer(decoder.codec, BUFFER_TIMEOUT_US);
        if (index >= 0) {
            size_t inputBufferSize;
            uint8_t *buf = AMediaCodec_getInputBuffer(decoder.codec, (size_t) index,
                                                      &inputBufferSize);
            // I have not seen any case where the input buffer returned by MediaCodec is too small to hold the NALU
            // But better be safe than crashing with a memory exception
            if (nalu.getSize() > inputBufferSize) {
                MLOGD << "Nalu too big" << nalu.getSize();
                return;
            }

            int flag = (IS_H265 && (nalu.isSPS() || nalu.isPPS() || nalu.isVPS()))
                       ? AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG : 0;
            std::memcpy(buf, nalu.getData(), (size_t) nalu.getSize());
            const uint64_t presentationTimeUS = (uint64_t) duration_cast<microseconds>(
                    steady_clock::now().time_since_epoch()).count();
            AMediaCodec_queueInputBuffer(decoder.codec, (size_t) index, 0, (size_t) nalu.getSize(),
                                         presentationTimeUS, flag);
            waitForInputB.add(steady_clock::now() - now);
            parsingTime.add(deltaParsing);
            return;
        } else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            //just try again. But if we had no success in the last 1 second,log a warning and return.
            const auto elapsedTimeTryingForBuffer = std::chrono::steady_clock::now() - now;
            if (elapsedTimeTryingForBuffer > std::chrono::seconds(1)) {
                // Since OpenHD provides a lossy link it is really unlikely, but possible that we somehow 'break' the codec by feeding corrupt data.
                // It will probably recover itself as soon as we feed enough valid data though;
                MLOGE << "AMEDIACODEC_INFO_TRY_AGAIN_LATER for more than 1 second "
                      << MyTimeHelper::R(elapsedTimeTryingForBuffer) << "return.";
                return;
            }
        } else {
            //Something went wrong. But we will feed the next NALU soon anyways
            MLOGD << "dequeueInputBuffer idx " << (int) index << "return.";
            return;
        }
    }
}

void VideoDecoder::checkOutputLoop() {
    NDKThreadHelper::setProcessThreadPriorityAttachDetach(javaVm, -16, "DecoderCheckOutput");
    AMediaCodecBufferInfo info;
    bool decoderSawEOS = false;
    bool decoderProducedUnknown = false;
    while (!decoderSawEOS && !decoderProducedUnknown) {
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(decoder.codec, &info,
                                                              BUFFER_TIMEOUT_US);
        if (index >= 0) {
            const auto now = steady_clock::now();
            const int64_t nowNS = (int64_t) duration_cast<nanoseconds>(
                    now.time_since_epoch()).count();
            const int64_t nowUS = (int64_t) duration_cast<microseconds>(
                    now.time_since_epoch()).count();
            //the timestamp for releasing the buffer is in NS, just release as fast as possible (e.g. now)
            //https://android.googlesource.com/platform/frameworks/av/+/master/media/ndk/NdkMediaCodec.cpp
            //-> renderOutputBufferAndRelease which is in https://android.googlesource.com/platform/frameworks/av/+/3fdb405/media/libstagefright/MediaCodec.cpp
            //-> Message kWhatReleaseOutputBuffer -> onReleaseOutputBuffer
            // also https://android.googlesource.com/platform/frameworks/native/+/5c1139f/libs/gui/SurfaceTexture.cpp
            AMediaCodec_releaseOutputBufferAtTime(decoder.codec, (size_t) index, nowNS);
            //but the presentationTime is in US
            decodingTime.add(std::chrono::microseconds(nowUS - info.presentationTimeUs));
            nDecodedFrames.add(1);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                MLOGD << "Decoder saw EOS";
                decoderSawEOS = true;
                continue;
            }
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            auto format = AMediaCodec_getOutputFormat(decoder.codec);
            int width = 0, height = 0;
            AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
            AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
            MLOGD << "Actual Width and Height in output " << width << "," << height;
            if (onDecoderRatioChangedCallback != nullptr && width != 0 && height != 0) {
                onDecoderRatioChangedCallback({width, height});
            }
            MLOGD << "AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED " << width << " " << height << " "
                  << AMediaFormat_toString(format);
        } else if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            MLOGD << "AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED";
        } else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            //MLOGD<<"AMEDIACODEC_INFO_TRY_AGAIN_LATER";
        } else {
            // Most like AMediaCodec_stop() was called
            MLOGD << "dequeueOutputBuffer idx: " << (int) index << " .Exit.";
            decoderProducedUnknown = true;
            continue;
        }
        //every 2 seconds recalculate the current fps and bitrate
        const auto now = steady_clock::now();
        const auto delta = now - decodingInfo.lastCalculation;
        if (delta > DECODING_INFO_RECALCULATION_INTERVAL) {
            decodingInfo.lastCalculation = steady_clock::now();
            decodingInfo.currentFPS = (float) nDecodedFrames.getDeltaSinceLastCall() /
                                      (float) duration_cast<seconds>(delta).count();
            decodingInfo.currentKiloBitsPerSecond = ((float) nNALUBytesFed.getDeltaSinceLastCall() /
                                                     duration_cast<seconds>(delta).count()) /
                                                    1024.0f * 8.0f;
            //and recalculate the avg latencies. If needed,also print the log.
            decodingInfo.avgDecodingTime_ms = decodingTime.getAvg_ms();
            decodingInfo.avgParsingTime_ms = parsingTime.getAvg_ms();
            decodingInfo.avgWaitForInputBTime_ms = waitForInputB.getAvg_ms();
            decodingInfo.nDecodedFrames = nDecodedFrames.getAbsolute();
            printAvgLog();
            if (onDecodingInfoChangedCallback != nullptr) {
                onDecodingInfoChangedCallback(decodingInfo);
            }
        }
    }
    MLOGD << "Exit CheckOutputLoop";
}

void VideoDecoder::printAvgLog() {
    if (PRINT_DEBUG_INFO) {
        auto now = steady_clock::now();
        if ((now - lastLog) > TIME_BETWEEN_LOGS) {
            lastLog = now;
            std::ostringstream frameLog;
            frameLog << std::fixed;
            float avgDecodingLatencySum =
                    decodingInfo.avgParsingTime_ms + decodingInfo.avgWaitForInputBTime_ms +
                    decodingInfo.avgDecodingTime_ms;
            frameLog << "......................Decoding Latency Averages......................" <<
                     "\nParsing:" << decodingInfo.avgParsingTime_ms
                     << " | WaitInputBuffer:" << decodingInfo.avgWaitForInputBTime_ms
                     << " | Decoding:" << decodingInfo.avgDecodingTime_ms
                     << " | Decoding Latency Sum:" << avgDecodingLatencySum <<
                     "\nN NALUS:" << decodingInfo.nNALU
                     << " | N NALUES feeded:" << decodingInfo.nNALUSFeeded << " | N Decoded Frames:"
                     << nDecodedFrames.getAbsolute() <<
                     "\nFPS:" << decodingInfo.currentFPS;
            MLOGD << frameLog.str();
        }
    }
}

void VideoDecoder::resetStatistics() {
    nDecodedFrames.reset();
    nNALUBytesFed.reset();
    parsingTime.reset();
    waitForInputB.reset();
    decodingTime.reset();
    decodingInfo = {};
}
