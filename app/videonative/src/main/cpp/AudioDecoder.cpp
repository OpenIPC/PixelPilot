//
// Created by Admin on 28/09/2024.
//

#include "AudioDecoder.h"
#include <android/log.h>

#define TAG "pixelpilot"

#define SAMPLE_RATE 48000
#define CHANNELS 1

AudioDecoder::AudioDecoder()
{
    initAudio();
}

AudioDecoder::~AudioDecoder()
{
    stopAudioProcessing();
    opus_decoder_destroy(pOpusDecoder);
    AAudioStream_requestStop(m_stream);
    AAudioStream_close(m_stream);
}

void AudioDecoder::enqueueAudio(const uint8_t* data, const std::size_t data_length)
{
    {
        std::lock_guard<std::mutex> lock(m_mtxQueue);
        m_audioQueue.push(AudioUDPPacket(data, data_length));
    }
    m_cvQueue.notify_one();
}

void AudioDecoder::processAudioQueue()
{
    while (true)
    {
        std::unique_lock<std::mutex> lock(m_mtxQueue);
        m_cvQueue.wait(lock, [this] { return !m_audioQueue.empty() || stopAudioFlag; });

        if (stopAudioFlag)
        {
            break;
        }
        if (!m_audioQueue.empty())
        {
            AudioUDPPacket audioPkt = m_audioQueue.front();
            onNewAudioData(audioPkt.data, audioPkt.len);
            m_audioQueue.pop();
            lock.unlock();
        }
    }
}

void AudioDecoder::initAudio()
{
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "initAudio");
    int error;
    pOpusDecoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
    // Create a stream m_builder
    AAudio_createStreamBuilder(&m_builder);

    // Set the stream format
    AAudioStreamBuilder_setFormat(m_builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(m_builder, CHANNELS);   // Mono
    AAudioStreamBuilder_setSampleRate(m_builder, SAMPLE_RATE);  // 8000 Hz

    AAudioStreamBuilder_setBufferCapacityInFrames(m_builder, BUFFER_CAPACITY_IN_FRAMES);

    // Open the stream
    AAudioStreamBuilder_openStream(m_builder, &m_stream);
    // Clean up the m_builder
    AAudioStreamBuilder_delete(m_builder);

    AAudioStream_requestStart(m_stream);

    isInit = true;
}

void AudioDecoder::stopAudio()
{
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "stopAudio");
    AAudioStream_requestStop(m_stream);
    AAudioStream_close(m_stream);
    isInit = false;
}

void AudioDecoder::onNewAudioData(const uint8_t* data, const std::size_t data_length)
{
    const int      rtp_header_size   = 12;
    const uint8_t* opus_payload      = data + rtp_header_size;
    int            opus_payload_size = data_length - rtp_header_size;

    int frame_size = opus_packet_get_samples_per_frame(opus_payload, SAMPLE_RATE);
    int nb_frames  = opus_packet_get_nb_frames(opus_payload, opus_payload_size);

    // Decode the frame
    int pcm_size = frame_size * nb_frames * CHANNELS;
    if (pOpusDecoder && m_stream)
    {
        opus_int16 pcm[pcm_size];
        int        decoded_samples = opus_decode(pOpusDecoder, opus_payload, opus_payload_size, pcm, pcm_size, 0);

        if (decoded_samples < 0)
        {
            return;
        }
        // Process the decoded PCM data
        AAudioStream_write(m_stream, pcm, decoded_samples, 0);
    }
}
