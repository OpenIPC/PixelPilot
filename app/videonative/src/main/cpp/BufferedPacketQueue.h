#if defined(__ANDROID__) || defined(__ANDROID_API__)
#include <android/log.h>
#else
#include <cassert>
#include <cstdio>
#endif
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include <unordered_map>
#include <vector>

// Define logging tag and maximum buffer size
#define BUFFERED_QUEUE_LOG_TAG "BufferedPacketQueue"
// Last-resort cap on the buffer. How long this is in wall clock time depends entirely on the
// packet rate, which is why it cannot be the only bound - see MAX_BUFFER_AGE.
constexpr size_t MAX_BUFFER_SIZE = 15;
// Number of monotonically increasing packets
constexpr size_t MONOTONIC_THRESHOLD = 5;
// A monotonic run is only tracked while the gap is plausibly a reorder. Kept separate from
// MONOTONIC_THRESHOLD: the gap grows by one with every packet held back, so gating the counter
// on the same value it is compared against means it can never reach it.
constexpr size_t MONOTONIC_MAX_DISTANCE = 64;

// Clock used to bound how long a packet may be held back.
using QueueClock     = std::chrono::steady_clock;
using QueueTimePoint = QueueClock::time_point;

// A gap in the sequence numbers on this path is almost always a packet that FEC could not
// recover, not a reorder: by the time packets get here they have come through wfb-ng and a
// loopback socket, where reordering takes microseconds. Waiting for a packet that will never
// arrive is pure added latency, so the wait is bounded in time rather than in packets - the
// packet bound alone is worth ~20ms on a 1080p video stream but ~300ms on the audio stream,
// which runs at a fraction of the packet rate.
constexpr auto MAX_BUFFER_AGE = std::chrono::milliseconds(20);

// Type definition for sequence numbers
using SeqType   = uint16_t;
using SignedSeq = std::make_signed_t<SeqType>;

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
    void processPacket(
        SeqType        currPacketIdx,
        const uint8_t* data,
        std::size_t    data_length,
        Callback&      callback,
        QueueTimePoint now = QueueClock::now())
    {
        logDebug(
            "Processing packet with Sequence=%u, lastPacketIdx=%u, firstPacket=%s",
            currPacketIdx,
            mLastPacketIdx,
            mFirstPacket ? "true" : "false");

        // Before anything else: give up on a gap we have been waiting on for too long. Done
        // here rather than in handleOutOfOrderPacket so that an in-order packet arriving after
        // a stall does not get delivered ahead of what is already buffered.
        if (!mPackets.empty() && (now - mOldestBufferedAt) >= MAX_BUFFER_AGE)
        {
            logWarning(
                "Held %zu packet(s) for more than %lldms waiting on Sequence=%u. Flushing.",
                mPackets.size(),
                (long long) MAX_BUFFER_AGE.count(),
                static_cast<unsigned>(static_cast<SeqType>(mLastPacketIdx + 1)));
            mLastPacketIdx = drainBufferInOrder(callback);
        }

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
            handleOutOfOrderPacket(currPacketIdx, data, data_length, callback, now);
        }
    }

  private:
    bool    mFirstPacket;
    SeqType mLastPacketIdx;

    std::unordered_map<SeqType, std::vector<uint8_t>> mPackets;

    // When the current stall started, i.e. when mPackets last went from empty to non-empty.
    QueueTimePoint mOldestBufferedAt{};

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
    bool isNextExpectedPacket(SeqType currPacketIdx) const
    {
        return currPacketIdx == static_cast<SeqType>(mLastPacketIdx + 1);
    }

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
    void handleOutOfOrderPacket(
        SeqType currPacketIdx, const uint8_t* data, std::size_t data_length, Callback& callback, QueueTimePoint now)
    {
        logDebug("Out-of-order packet detected. Sequence=%u", currPacketIdx);

        if (isDuplicatePacket(currPacketIdx))
        {
            logWarning("Duplicate packet received with Sequence=%u. ", currPacketIdx);
            // return;
        }

        bufferPacket(currPacketIdx, data, data_length, now);

        // calculateDistance(a, b) is how far b is ahead of a - see seqLessThan below - so the
        // question "is this packet ahead of the last one we delivered" has to be asked in that
        // order. Reversed, dist is negative for exactly the case this heuristic exists for (a
        // gap ahead of us), the else branch below clears the counter every time, and the
        // buffer only ever drains on the MAX_BUFFER_SIZE cap.
        auto dist = calculateDistance(mLastPacketIdx, currPacketIdx);
        if (static_cast<size_t>(std::abs(dist)) < MONOTONIC_MAX_DISTANCE)
        {
            // Check for monotonic increases
            if (dist > 0)
            {
                mMonotonicOutOfOrderIncreaseCount++;
                logDebug("Monotonic increase count: %zu", mMonotonicOutOfOrderIncreaseCount);
                if (mMonotonicOutOfOrderIncreaseCount >= MONOTONIC_THRESHOLD)
                {
                    mLastPacketIdx = drainBufferInOrder(callback);
                    logWarning("Monotonic threshold reached. Updating lastPacketIdx to %u", mLastPacketIdx);
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
            mLastPacketIdx = drainBufferInOrder(callback);
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
    void bufferPacket(SeqType currPacketIdx, const uint8_t* data, std::size_t data_length, QueueTimePoint now)
    {
        // Only the start of a stall is recorded, because the buffer is always drained as a
        // whole. A partial drain leaves the mark where it was, which errs towards flushing
        // early - the safe direction on a live link.
        if (mPackets.empty())
        {
            mOldestBufferedAt = now;
        }
        mPackets[currPacketIdx] = std::vector<uint8_t>(data, data + data_length);
        logDebug("Buffered out-of-order packet. Buffer size: %zu", mPackets.size());
    }

    /**
     * @brief Delivers everything currently held back, in sequence order, and empties the buffer.
     * @tparam Callback A callable type that processes the packet data.
     * @param callback Callable to handle processed packets.
     * @return The highest sequence number delivered, or mLastPacketIdx if nothing was held.
     */
    template <typename Callback>
    SeqType drainBufferInOrder(Callback& callback)
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

            // Sorted by distance from the last delivered packet, not by raw value: a block
            // that straddles the wrap point (65534, 65535, 0, 1) sorts to 0, 1, 65534, 65535
            // by value, and would be handed to the parser in that order.
            const SeqType from = mLastPacketIdx;
            std::sort(
                sortedPackets.begin(),
                sortedPackets.end(),
                [from](const auto& a, const auto& b)
                { return static_cast<SeqType>(a->first - from) < static_cast<SeqType>(b->first - from); });

            // Seeded from a packet that is actually in the buffer rather than from
            // mLastPacketIdx. RTP starts at a random sequence number, so a VTX that reboots
            // mid-session can land more than half the sequence space away, where
            // calculateDistance() reads as negative - seeded from mLastPacketIdx nothing
            // would ever move and the queue would never resync.
            SeqType highest = sortedPackets.front()->first;
            for (const auto& it : sortedPackets)
            {
                const auto& packet = it->second;
                logDebug("Processing possibly out-of-order buffered packet with Sequence=%u.", it->first);
                callback(packet.data(), packet.size());
                if (calculateDistance(highest, it->first) > 0)
                {
                    highest = it->first;
                }
            }

            mPackets.clear();
            // Reset the monotonic increase counter
            mMonotonicOutOfOrderIncreaseCount = 0;
            return highest;
        }
        return mLastPacketIdx;
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
#if defined(__ANDROID__) || defined(__ANDROID_API__)
        return;
        va_list args;
        va_start(args, format);
        __android_log_vprint(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG, format, args);
        va_end(args);
#else
        // Fallback to standard output for non-Android platforms
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fprintf(stderr, "\n");
#endif
    }

    /**
     * @brief Logs warning messages.
     * @param format printf-style format string.
     * @param ... Additional arguments.
     */
    void logWarning(const char* format, ...) const
    {
#if defined(__ANDROID__) || defined(__ANDROID_API__)
        va_list args;
        va_start(args, format);
        __android_log_vprint(ANDROID_LOG_WARN, BUFFERED_QUEUE_LOG_TAG, format, args);
        va_end(args);
#else
        // Fallback to standard output for non-Android platforms
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fprintf(stderr, "\n");
#endif
    }
};
