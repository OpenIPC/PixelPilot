//
// Created by BangDC on 28/09/2024.
//

#ifndef PIXELPILOT_AUDIODECODER_H
#define PIXELPILOT_AUDIODECODER_H
#include <aaudio/AAudio.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include "libs/include/opus.h"

typedef struct _AudioUDPPacket
{
    _AudioUDPPacket(const uint8_t* _data, size_t _len)
    {
        memcpy(data, _data, _len);
        len = _len;
    };
    uint8_t data[250];
    size_t  len;
} AudioUDPPacket;

class AudioDecoder
{
  public:
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

  private:
    const int                  BUFFER_CAPACITY_IN_FRAMES = (1024 + 256);
    std::queue<AudioUDPPacket> m_audioQueue;
    std::mutex                 m_mtxQueue;
    std::condition_variable    m_cvQueue;
    bool                       stopAudioFlag = false;
    std::thread                m_audioThread;
    AAudioStreamBuilder*       m_builder    = nullptr;
    AAudioStream*              m_stream     = nullptr;
    OpusDecoder*               pOpusDecoder = nullptr;
};
#endif  // PIXELPILOT_AUDIODECODER_H
