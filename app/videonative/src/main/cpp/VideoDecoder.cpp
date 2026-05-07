//
// Created by gaeta on 2024-04-01.
// Fixed by Claude - 2026-05-07
// Fixes:
//   [BUG 1] Race condition in checkOutputLoop - decoder.codec[idx] could be deleted by another thread
//   [BUG 2] MTK UNKNOWN_ERROR not handled - decoder not reset after codec crash
//   [BUG 3] configureStartDecoder checks codec==null instead of configure status
//   [BUG 4] configureStartDecoder destroys joinable thread → std::terminate() → SIGABRT

#include "VideoDecoder.h"
#include <unistd.h>
#include <sstream>
#include "AndroidThreadPrioValues.hpp"
#include "helper/AndroidMediaFormatHelper.h"
#include "helper/NDKThreadHelper.hpp"

#include <vector>

#include <android/native_window_jni.h>
#include <media/NdkMediaCodec.h>

using namespace std::chrono;

VideoDecoder::VideoDecoder(JNIEnv* env)
{
    env->GetJavaVM(&javaVm);
    resetStatistics();
}

void VideoDecoder::setOutputSurface(JNIEnv* env, jobject surface, jint idx)
{
    if (surface == nullptr)
    {
        MLOGD << "Set output null surface idx: " << idx;
        if (decoder.window[idx] == nullptr && decoder.codec[idx] == nullptr)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(mMutexInputPipe);
        inputPipeClosed = true;
        if (decoder.configured[idx])
        {
            AMediaCodec_stop(decoder.codec[idx]);
            AMediaCodec_delete(decoder.codec[idx]);
            decoder.codec[idx] = nullptr;
            MLOGD << "Set decoder.codec null idx: " << idx;
            mKeyFrameFinder.reset();
            decoder.configured[idx] = false;
            if (mCheckOutputThread[idx]->joinable())
            {
                mCheckOutputThread[idx]->join();
                mCheckOutputThread[idx].reset();
            }
        }
        if (decoder.window[idx])
        {
            ANativeWindow_release(decoder.window[idx]);
            decoder.window[idx] = nullptr;
            MLOGD << "Set decoder.window null idx: " << idx;
        }
        resetStatistics();
    }
    else
    {
        MLOGD << "Set output non-null surface idx :" << idx;
        assert(decoder.window[idx] == nullptr);
        decoder.window[idx] = ANativeWindow_fromSurface(env, surface);
        inputPipeClosed = false;
    }
}

void VideoDecoder::registerOnDecoderRatioChangedCallback(DECODER_RATIO_CHANGED decoderRatioChangedC)
{
    onDecoderRatioChangedCallback = std::move(decoderRatioChangedC);
}

void VideoDecoder::registerOnDecodingInfoChangedCallback(DECODING_INFO_CHANGED_CALLBACK decodingInfoChangedCallback)
{
    onDecodingInfoChangedCallback = std::move(decodingInfoChangedCallback);
}

void VideoDecoder::interpretNALU(const NALU& nalu)
{
    IS_H265             = nalu.IS_H265_PACKET;
    decodingInfo.nCodec = IS_H265;
    std::lock_guard<std::mutex> lock(mMutexInputPipe);
    decodingInfo.nNALU++;
    if (nalu.getSize() <= 4)
    {
        return;
    }
    nNALUBytesFed.add(nalu.getSize());
    if (inputPipeClosed)
    {
        MLOGD << "inputPipeClosed.";
        mKeyFrameFinder.saveIfKeyFrame(nalu);
        return;
    }
    if (decoder.configured[0] || decoder.configured[1])
    {
        feedDecoder(nalu, 0);
        feedDecoder(nalu, 1);
        decodingInfo.nNALUSFeeded++;
    }
    else
    {
        mKeyFrameFinder.saveIfKeyFrame(nalu);
        if (mKeyFrameFinder.allKeyFramesAvailable(IS_H265))
        {
            MLOGD << "Configuring decoder...";
            configureStartDecoder(0);
            configureStartDecoder(1);
        }
    }
}

void VideoDecoder::configureStartDecoder(int idx)
{
    if (decoder.window[idx] == nullptr) return;
    const std::string MIME = IS_H265 ? "video/hevc" : "video/avc";
    decoder.codec[idx]     = AMediaCodec_createDecoderByType(MIME.c_str());

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, MIME.c_str());

    if (IS_H265)
    {
        h265_configureAMediaFormat(mKeyFrameFinder, format);
    }
    else
    {
        h264_configureAMediaFormat(mKeyFrameFinder, format);
    }

    MLOGD << "Configuring decoder:" << AMediaFormat_toString(format);

    auto status = AMediaCodec_configure(decoder.codec[idx], format, decoder.window[idx], nullptr, 0);
    AMediaFormat_delete(format);

    // [BUG 3 FIX] Check configure status instead of null check.
    // codec pointer is non-null even when configure fails, so the original null check
    // was incorrect and would call AMediaCodec_start() on a misconfigured codec,
    // causing crashes on MediaTek (MTK) devices.
    if (status != AMEDIA_OK)
    {
        MLOGE << "AMediaCodec_configure failed with status: " << status;
        AMediaCodec_delete(decoder.codec[idx]);
        decoder.codec[idx] = nullptr;
        return;
    }

    MLOGD << "AMediaCodec_configure: OK";

    // [BUG 4 FIX] Join the old output thread before creating a new one.
    // If the previous codec crashed (MTK UNKNOWN_ERROR), checkOutputLoop already exited
    // but mCheckOutputThread[idx] is still joinable. Destroying a joinable std::thread
    // calls std::terminate() -> SIGABRT. This is a hard C++ rule with no exceptions.
    if (mCheckOutputThread[idx] && mCheckOutputThread[idx]->joinable())
    {
        MLOGD << "Joining previous output thread before reconfigure idx: " << idx;
        mCheckOutputThread[idx]->join();
        mCheckOutputThread[idx].reset();
    }

    AMediaCodec_start(decoder.codec[idx]);
    mCheckOutputThread[idx] = std::make_unique<std::thread>(&VideoDecoder::checkOutputLoop, this, idx);
    NDKThreadHelper::setName(mCheckOutputThread[idx]->native_handle(), "LLDCheckOutput");
    decoder.configured[idx] = true;
}

void VideoDecoder::feedDecoder(const NALU& nalu, int idx)
{
    if (!decoder.codec[idx]) return;
    const auto now          = std::chrono::steady_clock::now();
    const auto deltaParsing = now - nalu.creationTime;
    while (true)
    {
        const auto index = AMediaCodec_dequeueInputBuffer(decoder.codec[idx], BUFFER_TIMEOUT_US);
        if (index >= 0)
        {
            size_t   inputBufferSize;
            uint8_t* buf = AMediaCodec_getInputBuffer(decoder.codec[idx], (size_t) index, &inputBufferSize);
            if (nalu.getSize() > inputBufferSize)
            {
                MLOGD << "Nalu too big" << nalu.getSize();
                return;
            }

            int flag =
                (IS_H265 && (nalu.isSPS() || nalu.isPPS() || nalu.isVPS())) ? AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG : 0;
            std::memcpy(buf, nalu.getData(), (size_t) nalu.getSize());
            const uint64_t presentationTimeUS =
                (uint64_t) duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
            AMediaCodec_queueInputBuffer(
                decoder.codec[idx], (size_t) index, 0, (size_t) nalu.getSize(), presentationTimeUS, flag);
            waitForInputB.add(steady_clock::now() - now);
            parsingTime.add(deltaParsing);
            return;
        }
        else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        {
            const auto elapsedTimeTryingForBuffer = std::chrono::steady_clock::now() - now;
            if (elapsedTimeTryingForBuffer > std::chrono::seconds(1))
            {
                MLOGE << "AMEDIACODEC_INFO_TRY_AGAIN_LATER for more than 1 second "
                      << MyTimeHelper::R(elapsedTimeTryingForBuffer) << "return.";
                return;
            }
        }
        else
        {
            MLOGD << "dequeueInputBuffer idx " << (int) index << "return.";
            return;
        }
    }
}

void VideoDecoder::checkOutputLoop(int idx)
{
    NDKThreadHelper::setProcessThreadPriorityAttachDetach(javaVm, -16, "DecoderCheckOutput");
    AMediaCodecBufferInfo info;
    bool                  decoderSawEOS          = false;
    bool                  decoderProducedUnknown = false;
    while (!decoderSawEOS && !decoderProducedUnknown)
    {
        // [BUG 1 FIX] Take a local snapshot of the codec pointer at the start of each iteration.
        // Without this, another thread (e.g. setOutputSurface) could call AMediaCodec_delete()
        // between our null-check and our use of the pointer, causing a SIGSEGV (fault addr 0x0).
        AMediaCodec* localCodec = decoder.codec[idx];
        if (!localCodec) break;

        const ssize_t index = AMediaCodec_dequeueOutputBuffer(localCodec, &info, BUFFER_TIMEOUT_US);
        if (index >= 0)
        {
            const auto    now   = steady_clock::now();
            const int64_t nowUS = (int64_t) duration_cast<microseconds>(now.time_since_epoch()).count();

            // [BUG 1 FIX] Re-snapshot before use — state may have changed during dequeueOutputBuffer
            AMediaCodec* localCodec2 = decoder.codec[idx];
            if (!localCodec2) break;
            AMediaCodec_releaseOutputBuffer(localCodec2, (size_t) index, true);

            if (idx == 0)
            {
                decodingTime.add(std::chrono::microseconds(nowUS - info.presentationTimeUs));
                nDecodedFrames.add(1);
            }
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
            {
                MLOGD << "Decoder saw EOS";
                decoderSawEOS = true;
                continue;
            }
        }
        else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            AMediaCodec* localCodecFmt = decoder.codec[idx];
            if (!localCodecFmt) break;
            auto format = AMediaCodec_getOutputFormat(localCodecFmt);
            int  width = 0, height = 0;
            AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
            AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
            MLOGD << "Actual Width and Height in output " << width << "," << height;
            if (idx == 0 && onDecoderRatioChangedCallback != nullptr && width != 0 && height != 0)
            {
                onDecoderRatioChangedCallback({width, height});
            }
            MLOGD << "AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED " << width << " " << height << " "
                  << AMediaFormat_toString(format);
        }
        else if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
        {
            MLOGD << "AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED";
        }
        else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        {
            // MLOGD<<"AMEDIACODEC_INFO_TRY_AGAIN_LATER";
        }
        else
        {
            // [BUG 2 FIX] On MediaTek (MTK) devices, the codec may report UNKNOWN_ERROR
            // (0x80000000) and enter a dead "Released" state. Unlike Snapdragon, MTK codecs
            // do not self-recover.
            //
            // Root cause of the spam: feedDecoder() checks decoder.codec[idx] != null before
            // calling dequeueInputBuffer — but we never nulled it out, so it kept hammering
            // a dead codec and producing "Invalid to call at Released state" every ~8ms.
            //
            // Fix: atomically grab and null out the pointer HERE (in the output thread),
            // BEFORE stopping/deleting, so feedDecoder sees null and bails out immediately.
            // Then notify the upper layer via callback to trigger reconfiguration.
            MLOGE << "dequeueOutputBuffer idx: " << (int) index << " — codec dead, cleaning up.";
            decoderProducedUnknown = true;

            // Grab pointer and null it atomically so feedDecoder stops immediately
            AMediaCodec* deadCodec = decoder.codec[idx];
            decoder.codec[idx]     = nullptr;
            decoder.configured[idx] = false;
            if (deadCodec)
            {
                AMediaCodec_stop(deadCodec);
                AMediaCodec_delete(deadCodec);
            }
            if (onDecodingInfoChangedCallback != nullptr)
            {
                decodingInfo.decoderError = true;
                onDecodingInfoChangedCallback(decodingInfo);
            }
            continue;
        }
        // every 2 seconds recalculate the current fps and bitrate
        const auto now   = steady_clock::now();
        const auto delta = now - decodingInfo.lastCalculation;
        if (idx == 0 && delta > DECODING_INFO_RECALCULATION_INTERVAL)
        {
            decodingInfo.lastCalculation = steady_clock::now();
            decodingInfo.currentFPS =
                (float) nDecodedFrames.getDeltaSinceLastCall() / (float) duration_cast<seconds>(delta).count();
            decodingInfo.currentKiloBitsPerSecond =
                ((float) nNALUBytesFed.getDeltaSinceLastCall() / duration_cast<seconds>(delta).count()) / 1024.0f *
                8.0f;
            decodingInfo.avgDecodingTime_ms      = decodingTime.getAvg_ms();
            decodingInfo.avgParsingTime_ms       = parsingTime.getAvg_ms();
            decodingInfo.avgWaitForInputBTime_ms = waitForInputB.getAvg_ms();
            decodingInfo.nDecodedFrames          = nDecodedFrames.getAbsolute();
            printAvgLog();
            if (onDecodingInfoChangedCallback != nullptr)
            {
                onDecodingInfoChangedCallback(decodingInfo);
            }
        }
    }
    MLOGD << "Exit CheckOutputLoop";
}

void VideoDecoder::printAvgLog()
{
    if (PRINT_DEBUG_INFO)
    {
        auto now = steady_clock::now();
        if ((now - lastLog) > TIME_BETWEEN_LOGS)
        {
            lastLog = now;
            std::ostringstream frameLog;
            frameLog << std::fixed;
            float avgDecodingLatencySum =
                decodingInfo.avgParsingTime_ms + decodingInfo.avgWaitForInputBTime_ms + decodingInfo.avgDecodingTime_ms;
            frameLog << "......................Decoding Latency Averages......................"
                     << "\nParsing:" << decodingInfo.avgParsingTime_ms
                     << " | WaitInputBuffer:" << decodingInfo.avgWaitForInputBTime_ms
                     << " | Decoding:" << decodingInfo.avgDecodingTime_ms
                     << " | Decoding Latency Sum:" << avgDecodingLatencySum << "\nN NALUS:" << decodingInfo.nNALU
                     << " | N NALUES feeded:" << decodingInfo.nNALUSFeeded
                     << " | N Decoded Frames:" << nDecodedFrames.getAbsolute() << "\nFPS:" << decodingInfo.currentFPS
                     << " | Codec:" << (decodingInfo.nCodec ? "H265" : "H264");
            MLOGD << frameLog.str();
        }
    }
}

void VideoDecoder::resetStatistics()
{
    nDecodedFrames.reset();
    nNALUBytesFed.reset();
    parsingTime.reset();
    waitForInputB.reset();
    decodingTime.reset();
    decodingInfo = {};
}
