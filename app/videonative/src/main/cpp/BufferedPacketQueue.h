#pragma once
#include <map>
#define BUFFERED_QUEUE_LOG_TAG "BufferedPacketQueue"

/**
 * @class BufferedPacketQueue
 * @brief A queue for managing and processing network packets in sequence order.
 * This class leverages a buffer to handle out-of-order packets and ensures
 * packets are processed sequentially.
 */
class BufferedPacketQueue {
  // Using std::map to keep packets sorted by sequence number
  template <typename SeqType> class PacketBuffer {
  public:
    std::map<SeqType, std::vector<uint8_t>> packets;
    SeqType lastPacketIdx;
    static constexpr std::size_t MAX_BUFFER_SIZE = 20;
    bool firstPacket;

    // Constructor with logging
    PacketBuffer() : lastPacketIdx(0), firstPacket(true) {
      __android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG, "PacketBuffer initialized.");
    }

    /**
     * @brief Compares two sequence numbers considering wrap-around.
     * @param a First sequence number.
     * @param b Second sequence number.
     * @return True if sequence a is less than b, accounting for wrap-around.
     */
    bool seqLessThan(SeqType a, SeqType b) const {
      // Calculate the midpoint based on the sequence number width
      const SeqType midpoint = (std::numeric_limits<SeqType>::max() / 2) + 1;
      bool result =
          ((b > a) && (b - a < midpoint)) || ((a > b) && (a - b > midpoint));
      //            __android_log_print(ANDROID_LOG_VERBOSE, BUFFERED_QUEUE_LOG_TAG, "Comparing
      //            sequences: a=%u, b=%u, a < b? %s",
      //                                a, b, result ? "true" : "false");
      return result;
    }

    /**
     * @brief Processes an incoming packet based on its sequence index.
     * @param idx Sequence index of the packet.
     * @param data Pointer to packet data.
     * @param data_length Size of packet data.
     * @param callback Callable to handle processed packets.
     */
    template <typename Callback>
    void processPacket(SeqType idx, const uint8_t *data,
                       std::size_t data_length, Callback &callback) {
      //            __android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG, "Processing
      //            packet with Sequence=%u", idx);

      if (firstPacket) {
        // Initialize lastPacketIdx to one before the first packet
        lastPacketIdx = idx - 1;
        firstPacket = false;
        __android_log_print(
            ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG,
            "First packet received. Initialized lastPacketIdx to %u",
            lastPacketIdx);
      }

      if (idx == lastPacketIdx + 1) {
        // Packet is the next expected one
        //                __android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG, "In-order
        //                packet detected. Processing immediately.");
        callback(data, data_length);
        lastPacketIdx = idx;
        //                __android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG, "Updated
        //                lastPacketIdx to %u", lastPacketIdx);

        // Now check if the buffer has the next packets
        while (true) {
          auto it = packets.find(lastPacketIdx + 1);
          if (it != packets.end()) {
            __android_log_print(
                ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG,
                "Found buffered packet with Sequence=%u. Processing.",
                it->first);
            callback(it->second.data(), it->second.size());
            lastPacketIdx = it->first;
            __android_log_print(
                ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG,
                "Updated lastPacketIdx to %u after processing buffered packet.",
                lastPacketIdx);
            packets.erase(it);
          } else {
            //__android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG, "No buffered packet
            // found for Sequence=%u.", lastPacketIdx + 1);
            break;
          }
        }
      } else if (seqLessThan(lastPacketIdx, idx)) {
        // Out-of-order packet
        __android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG,
                            "Out-of-order packet detected. Sequence=%u", idx);
        // Avoid duplicate packets
        if (packets.find(idx) == packets.end()) {
          // Buffer the packet
          packets[idx] = std::vector<uint8_t>(data, data + data_length);
          __android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG,
                              "Buffered out-of-order packet. Buffer size: %zu",
                              packets.size());

          // If buffer size exceeds MAX_BUFFER_SIZE, process all buffered
          // packets
          if (packets.size() >= MAX_BUFFER_SIZE) {
            __android_log_print(ANDROID_LOG_WARN, BUFFERED_QUEUE_LOG_TAG,
                                "Buffer size exceeded MAX_BUFFER_SIZE (%zu). "
                                "Processing all buffered packets.",
                                MAX_BUFFER_SIZE);
            // Process buffered packets in order
            auto it_buffer = packets.begin();
            while (it_buffer != packets.end()) {
              __android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG,
                                  "Processing buffered packet with Sequence=%u",
                                  it_buffer->first);
              callback(it_buffer->second.data(), it_buffer->second.size());
              lastPacketIdx = it_buffer->first;
              it_buffer = packets.erase(it_buffer);
              __android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG,
                                  "Updated lastPacketIdx to %u after "
                                  "processing buffered packet.",
                                  lastPacketIdx);
            }
          }
        } else {
          __android_log_print(
              ANDROID_LOG_WARN, BUFFERED_QUEUE_LOG_TAG,
              "Duplicate packet received with Sequence=%u. Ignoring.", idx);
        }
      } else {
        // Packet is older than lastPacketIdx, possibly a retransmission or
        // duplicate
        __android_log_print(
            ANDROID_LOG_WARN, BUFFERED_QUEUE_LOG_TAG,
            "Received old or duplicate packet with Sequence=%u. Ignoring.",
            idx);
        // Optionally, handle retransmissions or request retransmission here
      }
    }
  };

  // Decide whether to use uint8_t or uint16_t based on sequence number type
  using SeqType = uint16_t; // Change to uint8_t if sequence numbers are 8-bit

  PacketBuffer<SeqType> buffer;

public:
  /**
   * @brief Process a packet through the queue.
   * @param idx Sequence index of the packet.
   * @param data Pointer to packet data.
   * @param data_length Size of the packet data.
   * @param callback Callable to handle processed packets.
   */
  template <typename Callback>
  void processPacket(SeqType idx, const uint8_t *data, std::size_t data_length,
                     Callback &callback) {
    //__android_log_print(ANDROID_LOG_DEBUG, BUFFERED_QUEUE_LOG_TAG, "BufferedPacketQueue:
    // Processing packet with Sequence=%u", idx);
    buffer.processPacket(idx, data, data_length, callback);
  }
};
