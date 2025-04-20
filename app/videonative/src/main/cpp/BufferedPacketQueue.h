#include <android/log.h>
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

// Define logging tag and maximum buffer size
#define BUFFERED_QUEUE_LOG_TAG "BufferedPacketQueue"
// Considering the packet rate about 100 packets per second, 10 packets should be enough
constexpr size_t MAX_BUFFER_SIZE = 5;
// Number of monotonically increasing packets
constexpr size_t MONOTONIC_THRESHOLD = 3;

// Type definition for sequence numbers
using SeqType = uint16_t;

/**
 * @brief BufferedPacketQueue class handles packet processing with sequence numbers,
 *        ensuring in-order delivery and buffering out-of-order packets.
 */
class BufferedPacketQueue
{
  public:
    /**
     * @brief Constructs a BufferedPacketQueue instance.
     */
    BufferedPacketQueue() : mFirstPacket(true), mLastPacketIdx(0), mMonotonicOutOfOrderIncreaseCount(0) {}

    /**
     * @brief Processes an incoming packet based on its sequence index.
     * @tparam Callback A callable type that processes the packet data.
     * @param currPacketIdx Sequence index of the incoming packet.
     * @param data Pointer to the packet data.
     * @param data_length Size of the packet data.
     * @param callback Callable to handle processed packets.
     */
    template <typename Callback>
    void processPacket(SeqType currPacketIdx, const uint8_t* data, std::size_t data_length, Callback& callback)
    {
        logDebug(
            "Processing packet with Sequence=%u, lastPacketIdx=%u, firstPacket=%s",
            currPacketIdx,
            mLastPacketIdx,
            mFirstPacket ? "true" : "false");

        if (isFirstPacket(currPacketIdx))
        {
            handleFirstPacket(currPacketIdx);
            // Continue processing the first packet
            processInOrderPacket(currPacketIdx, data, data_length, callback);
            processBufferedPackets(callback);
            return;
        }

        if (isNextExpectedPacket(currPacketIdx))
        {
            // In-order packet
            processInOrderPacket(currPacketIdx, data, data_length, callback);
            processBufferedPackets(callback);
            // Reset monotonic increase counter after in-order packet
            mMonotonicOutOfOrderIncreaseCount = 0;
        }
        else
        {
            // Out-of-order packet
            handleOutOfOrderPacket(currPacketIdx, data, data_length, callback);
        }
    }

  private:
    bool    mFirstPacket;
    SeqType mLastPacketIdx;

    std::unordered_map<SeqType, std::vector<uint8_t>> mPackets;

    // This variable is used to track a situation where the sequence number is increasing monotonically while packets
    // are out of order. if this counter reaches MONOTONIC_THRESHOLD, we will restart buffering and update lastPacketIdx
    // to the highest sequence index received.
    size_t mMonotonicOutOfOrderIncreaseCount;

    /**
     * @brief Determines if the incoming packet is the first packet.
     * @param currPacketIdx Sequence index of the incoming packet.
     * @return True if it's the first packet; otherwise, false.
     */
    bool isFirstPacket(SeqType currPacketIdx) const { return mFirstPacket; }

    /**
     * @brief Handles the first packet by initializing the lastPacketIdx.
     * @param currPacketIdx Sequence index of the first packet.
     */
    void handleFirstPacket(SeqType currPacketIdx)
    {
        mLastPacketIdx = currPacketIdx - 1;
        mFirstPacket   = false;
        logDebug("First packet received. Initialized lastPacketIdx to %u", mLastPacketIdx);
    }

    /**
     * @brief Checks if the incoming packet is the next expected in order.
     * @param currPacketIdx Sequence index of the incoming packet.
     * @return True if it's the next expected packet; otherwise, false.
     */
    bool isNextExpectedPacket(SeqType currPacketIdx) const { return currPacketIdx == mLastPacketIdx + 1; }

    /**
     * @brief Processes an in-order packet by invoking the callback and updating state.
     * @tparam Callback A callable type that processes the packet data.
     * @param currPacketIdx Sequence index of the packet.
     * @param data Pointer to the packet data.
     * @param data_length Size of the packet data.
     * @param callback Callable to handle processed packets.
     */
    template <typename Callback>
    void processInOrderPacket(SeqType currPacketIdx, const uint8_t* data, std::size_t data_length, Callback& callback)
    {
        logDebug("In-order packet detected. Processing immediately.");

        // in-order packet receiver which means we restart tracking out of order monotonic increases
        mMonotonicOutOfOrderIncreaseCount = 0;

        callback(data, data_length);
        mLastPacketIdx = currPacketIdx;
        logDebug("Updated lastPacketIdx to %u", mLastPacketIdx);
    }

    /**
     * @brief Processes buffered packets that can now be delivered in order.
     * @tparam Callback A callable type that processes the packet data.
     * @param callback Callable to handle processed packets.
     */
    template <typename Callback>
    void processBufferedPackets(Callback& callback)
    {
        while (true)
        {
            SeqType nextIdx = mLastPacketIdx + 1;
            auto    it      = mPackets.find(nextIdx);
            if (it != mPackets.end())
            {
                logDebug("Found buffered packet with Sequence=%u. Processing.", it->first);
                callback(it->second.data(), it->second.size());
                mLastPacketIdx = it->first;
                logDebug("Updated lastPacketIdx to %u after processing buffered packet.", mLastPacketIdx);
                mPackets.erase(it);
            }
            else
            {
                logDebug("No buffered packet found for Sequence=%u.", nextIdx);
                break;
            }
        }
    }

    /**
     * @brief Handles out-of-order packets by buffering or ignoring based on distance and monotonic increases.
     * @tparam Callback A callable type that processes the packet data.
     * @param currPacketIdx Sequence index of the incoming packet.
     * @param data Pointer to the packet data.
     * @param data_length Size of the packet data.
     * @param callback Callable to handle processed packets.
     */
    template <typename Callback>
    void handleOutOfOrderPacket(SeqType currPacketIdx, const uint8_t* data, std::size_t data_length, Callback& callback)
    {
        logDebug("Out-of-order packet detected. Sequence=%u", currPacketIdx);

        if (isDuplicatePacket(currPacketIdx))
        {
            logWarning("Duplicate packet received with Sequence=%u. Ignoring.", currPacketIdx);
            return;
        }

        bufferPacket(currPacketIdx, data, data_length);

        auto dist = calculateDistance(currPacketIdx, mLastPacketIdx);
        if (std::abs(dist) < MONOTONIC_THRESHOLD)
        {
            // Check for monotonic increases
            if (dist > 0)
            {
                mMonotonicOutOfOrderIncreaseCount++;
                logDebug("Monotonic increase count: %zu", mMonotonicOutOfOrderIncreaseCount);
                if (mMonotonicOutOfOrderIncreaseCount >= MONOTONIC_THRESHOLD)
                {
                    restartBuffering(callback, currPacketIdx);
                    // Update lastPacketIdx to the highest sequence index received
                    SeqType newLastIdx = currPacketIdx;
                    logWarning("Monotonic threshold reached. Updating lastPacketIdx to %u", newLastIdx);
                }
            }
            else
            {
                // Reset the counter if a non-increasing packet is received
                mMonotonicOutOfOrderIncreaseCount = 0;
                logDebug("Non-increasing packet received. Resetting monotonic increase count.");
            }
        }
        // If buffer size exceeds MAX_BUFFER_SIZE, handle buffer overflow
        if (mPackets.size() >= MAX_BUFFER_SIZE)
        {
            logWarning(
                "Buffer size exceeded MAX_BUFFER_SIZE (%zu). Processing in-order buffered packets.", MAX_BUFFER_SIZE);
            restartBuffering(callback, currPacketIdx);
        }
    }

    /**
     * @brief Checks if the incoming packet is a duplicate.
     * @param currPacketIdx Sequence index of the incoming packet.
     * @return True if the packet is a duplicate; otherwise, false.
     */
    bool isDuplicatePacket(SeqType currPacketIdx) const { return mPackets.find(currPacketIdx) != mPackets.end(); }

    /**
     * @brief Buffers an out-of-order packet.
     * @param currPacketIdx Sequence index of the incoming packet.
     * @param data Pointer to the packet data.
     * @param data_length Size of the packet data.
     */
    void bufferPacket(SeqType currPacketIdx, const uint8_t* data, std::size_t data_length)
    {
        mPackets[currPacketIdx] = std::vector<uint8_t>(data, data + data_length);
        logDebug("Buffered out-of-order packet. Buffer size: %zu", mPackets.size());
    }

    /**
     * @brief Handles buffer overflow by processing in-order packets and discarding others.
     * @tparam Callback A callable type that processes the packet data.
     * @param callback Callable to handle processed packets.
     */
    template <typename Callback>
    void restartBuffering(Callback& callback, SeqType currPacketIdx)
    {
        // Process as many in-order buffered packets as possible
        processBufferedPackets(callback);

        if (!mPackets.empty())
        {
            logWarning("Processing %zu buffered packets that might be out of order.", mPackets.size());

            // Create a vector of iterators to the map elements
            std::vector<std::unordered_map<uint16_t, std::vector<uint8_t>>::const_iterator> sortedPackets;
            sortedPackets.reserve(mPackets.size());

            // Populate the vector with iterators to the map elements
            for (auto it = mPackets.cbegin(); it != mPackets.cend(); ++it)
            {
                sortedPackets.push_back(it);
            }

            // Sort the vector based on the keys
            std::sort(
                sortedPackets.begin(),
                sortedPackets.end(),
                [](const auto& a, const auto& b) { return a->first < b->first; });

            // Iterate over the sorted packets and invoke the callback
            for (const auto& it : sortedPackets)
            {
                const auto& packet = it->second;
                logDebug("Processing possibly out-of-order buffered packet with Sequence=%u.", it->first);
                callback(packet.data(), packet.size());
            }

            mPackets.clear();
            // Reset the monotonic increase counter
            mMonotonicOutOfOrderIncreaseCount = 0;
        }
        mLastPacketIdx = currPacketIdx;
    }

    /**
     * @brief Compares two sequence numbers considering wrap-around.
     * @param a First sequence number.
     * @param b Second sequence number.
     * @return True if sequence a is less than b, accounting for wrap-around.
     */
    bool seqLessThan(SeqType a, SeqType b) const
    {
        bool result = calculateDistance(a, b) > 0;
        logDebug("seqLessThan: a=%u, b=%u, result=%s", a, b, result ? "true" : "false");
        return result;
    }

    template <typename T>
    typename std::make_signed<T>::type to_signed(T value)
    {
        static_assert(std::is_unsigned<T>::value, "Type must be unsigned");
        using SignedType = typename std::make_signed<T>::type;
        return static_cast<SignedType>(value);
    }

    /**
     * @brief Calculates the distance between two sequence numbers, considering wrap-around.
     * @param from Starting sequence number.
     * @param to Destination sequence number.
     * @return The distance from 'from' to 'to'.
     */
    static std::make_signed<SeqType>::type calculateDistance(SeqType a, SeqType b)
    {
        return static_cast<std::make_signed<SeqType>::type>(b - a);
    }

    /**
     * @brief Logs debug messages.
     * @param format printf-style format string.
     * @param ... Additional arguments.
     */
    void logDebug(const char* format, ...) const
    {
        return;
        va_list args;
        va_start(args, format);
        __android_log_vprint(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG, format, args);
        va_end(args);
    }

    /**
     * @brief Logs warning messages.
     * @param format printf-style format string.
     * @param ... Additional arguments.
     */
    void logWarning(const char* format, ...) const
    {
        va_list args;
        va_start(args, format);
        __android_log_vprint(ANDROID_LOG_WARN, BUFFERED_QUEUE_LOG_TAG, format, args);
        va_end(args);
    }
};
