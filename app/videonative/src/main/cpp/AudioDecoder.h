//
// Created by BangDC on 28/09/2024.
//

#ifndef PIXELPILOT_AUDIODECODER_H
#define PIXELPILOT_AUDIODECODER_H
#include <aaudio/AAudio.h>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include "libs/include/opus.h"

typedef struct _AudioUDPPacket
{
    // Room for any RTP packet the link delivers. It was 250 bytes with nothing checking the
    // length, so a perfectly ordinary packet - PCM at 8 kHz is already 320 bytes per 20 ms -
    // was written past the end of the array. enqueueAudio() drops anything larger.
    static constexpr size_t kMaxLen = 1500;

    _AudioUDPPacket(const uint8_t* _data, size_t _len) : len(_len <= kMaxLen ? _len : 0)
    {
        if (len > 0)
        {
            memcpy(data, _data, len);
        }
    };
    uint8_t data[kMaxLen];
    size_t  len;
} AudioUDPPacket;

class AudioDecoder
{
  public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels   = 1;
    // The longest an Opus packet can be is 120 ms, which at 48 kHz is 5760 samples per
    // channel - so a buffer this size holds any packet opus_decode() will accept.
    static constexpr int kMaxFrameSamples = 5760;

    AudioDecoder();
    ~AudioDecoder();

    // Audio buffer
    void initAudio();
    void enqueueAudio(const uint8_t* data, const std::size_t data_length);
    void startAudioProcessing()
    {
        stopAudioFlag = false;
        m_audioThread = std::thread(&AudioDecoder::processAudioQueue, this);
    }

    void stopAudioProcessing()
    {
        {
            std::lock_guard<std::mutex> lock(m_mtxQueue);
            stopAudioFlag = true;
        }
        m_cvQueue.notify_all();
        if (m_audioThread.joinable())
        {
            m_audioThread.join();
        }
    }
    void processAudioQueue();
    void stopAudio();
    bool isInit = false;

  private:
    void onNewAudioData(const uint8_t* data, const std::size_t data_length);
    // Both expect m_mtxStream to be held.
    bool openOutputLocked();
    void closeOutputLocked();

  private:
    const int                  BUFFER_CAPACITY_IN_FRAMES = 4096;
    std::queue<AudioUDPPacket> m_audioQueue;
    std::mutex                 m_mtxQueue;
    std::condition_variable    m_cvQueue;
    bool                       stopAudioFlag = false;
    std::thread                m_audioThread;
    // Guards m_stream. The decode thread writes to it while stopAudio() closes it from another
    // thread - and VideoActivity.onPause() calls VideoPlayer.stop(), which closes the stream,
    // before stopAudio() has stopped that thread.
    std::mutex                 m_mtxStream;
    AAudioStream*              m_stream     = nullptr;
    OpusDecoder*               pOpusDecoder = nullptr;
    // Only ever touched by the decode thread.
    opus_int16                 m_pcm[kMaxFrameSamples * kChannels];
};
#endif  // PIXELPILOT_AUDIODECODER_H
