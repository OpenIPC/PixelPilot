//
// Created by Admin on 28/09/2024.
//

#include "AudioDecoder.h"
#include <android/log.h>

#define TAG "pixelpilot"

AudioDecoder::AudioDecoder()
{
    initAudio();
}

AudioDecoder::~AudioDecoder()
{
    stopAudioProcessing();
    {
        std::lock_guard<std::mutex> lock(m_mtxStream);
        closeOutputLocked();
    }
    // opus_decoder_create() allocates with malloc, so this is the only correct way to free it;
    // the delete that used to be here was undefined behaviour.
    if (pOpusDecoder != nullptr)
    {
        opus_decoder_destroy(pOpusDecoder);
        pOpusDecoder = nullptr;
    }
}

void AudioDecoder::enqueueAudio(const uint8_t* data, const std::size_t data_length)
{
    // Dropped rather than truncated: a cut Opus packet does not fail to decode, it decodes to
    // noise. Nothing either receiver delivers is this long; see AudioUDPPacket::kMaxLen.
    if (data_length == 0 || data_length > AudioUDPPacket::kMaxLen)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mtxQueue);
        // Bounded, because decoding runs without this lock now and nothing else holds the
        // receive thread back. If the decoder ever falls behind, the oldest packet goes:
        // played late it would only be latency.
        if (m_audioQueue.size() >= kMaxQueued)
        {
            m_audioQueue.pop();
        }
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
        AudioUDPPacket audioPkt = std::move(m_audioQueue.front());
        m_audioQueue.pop();
        // Decoded with the queue released. enqueueAudio() is called on the receive thread, which
        // carries the video as well, and it used to wait for every decode to finish.
        lock.unlock();
        onNewAudioData(audioPkt.data.data(), audioPkt.data.size());
    }
}

void AudioDecoder::initAudio()
{
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "initAudio");
    // Only once: start after stop() comes back through here, and creating a second decoder
    // leaked the first.
    if (pOpusDecoder == nullptr)
    {
        int error    = OPUS_OK;
        pOpusDecoder = opus_decoder_create(kSampleRate, kChannels, &error);
        if (error != OPUS_OK || pOpusDecoder == nullptr)
        {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "opus decoder could not be created: %s",
                                opus_strerror(error));
            pOpusDecoder = nullptr;
            isInit       = false;
            return;
        }
    }
    std::lock_guard<std::mutex> lock(m_mtxStream);
    isInit = openOutputLocked();
}

bool AudioDecoder::openOutputLocked()
{
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t      result  = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK)
    {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "audio stream builder failed: %s",
                            AAudio_convertResultToText(result));
        return false;
    }

    // The camera may encode at 8 kHz; that does not matter here, because an Opus decoder
    // renders any Opus stream at whichever of its rates it was created for.
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, kChannels);
    AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, BUFFER_CAPACITY_IN_FRAMES);

    result = AAudioStreamBuilder_openStream(builder, &m_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK)
    {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "audio stream could not be opened: %s",
                            AAudio_convertResultToText(result));
        m_stream = nullptr;
        return false;
    }

    result = AAudioStream_requestStart(m_stream);
    if (result != AAUDIO_OK)
    {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "audio stream would not start: %s",
                            AAudio_convertResultToText(result));
        AAudioStream_close(m_stream);
        m_stream = nullptr;
        return false;
    }
    return true;
}

void AudioDecoder::closeOutputLocked()
{
    if (m_stream != nullptr)
    {
        AAudioStream_requestStop(m_stream);
        AAudioStream_close(m_stream);
        // Cleared, so nothing writes to a closed stream and the destructor cannot close it a
        // second time - which is what stopAudio() followed by destruction used to do.
        m_stream = nullptr;
    }
}

void AudioDecoder::stopAudio()
{
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "stopAudio");
    std::lock_guard<std::mutex> lock(m_mtxStream);
    closeOutputLocked();
    isInit = false;
}

void AudioDecoder::onNewAudioData(const uint8_t* data, const std::size_t data_length)
{
    // Everything below is read from the radio, so every length is checked before it is used.
    // The payload used to be taken from a fixed offset of 12 with no check at all: a packet
    // shorter than that gave a negative size, which then sized a stack array.
    constexpr std::size_t kFixedHeader = 12;
    if (data_length < kFixedHeader)
    {
        return;
    }

    // The real header length: CSRCs and an extension both lengthen it, and reading from a
    // fixed offset would decode the end of the header as audio.
    std::size_t header = kFixedHeader + 4 * (data[0] & 0x0F);
    if (data[0] & 0x10)
    {
        if (data_length < header + 4)
        {
            return;
        }
        const std::size_t words = (static_cast<std::size_t>(data[header + 2]) << 8) | data[header + 3];
        header += 4 + 4 * words;
    }
    if (data_length <= header)
    {
        return;
    }

    std::size_t payload = data_length - header;
    if (data[0] & 0x20)
    {
        // Padding, with its own length in the last byte.
        const uint8_t padding = data[data_length - 1];
        if (padding >= payload)
        {
            return;
        }
        payload -= padding;
    }

    if (pOpusDecoder == nullptr)
    {
        return;
    }
    // Into a fixed buffer sized for the longest packet Opus allows, rather than one sized from
    // what the packet claims about itself.
    const int decoded = opus_decode(pOpusDecoder, data + header, static_cast<opus_int32>(payload), m_pcm,
                                    kMaxFrameSamples, 0);
    if (decoded <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mtxStream);
    if (m_stream != nullptr)
    {
        AAudioStream_write(m_stream, m_pcm, decoded, 0);
    }
}
