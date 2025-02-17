#pragma once

// -- External C Libraries --

extern "C" {
#include "wfb-ng/src/fec.h" // FEC library
}

#include "devourer/src/Rtl8812aDevice.h" // Rtl8812aDevice definition
#include "wfb-ng/src/wifibroadcast.hpp"  // Wifibroadcast definitions

// -- System / C++ Includes --
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/random.h>
#include <memory>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>

//// For Android logging
// #ifdef __ANDROID__
// #include <android/log.h>
// #define TAG "TxFrame"
// #endif

// //-------------------------------------------------------------
// // Utility function to format strings (variadic).

// static std::string string_format(const char* fmt, ...) {
//     va_list args;
//     va_start(args, fmt);
//     char buffer[1024];
//     vsnprintf(buffer, sizeof(buffer), fmt, args);
//     va_end(args);
//     return std::string(buffer);
// }

//-------------------------------------------------------------
// A custom deleter for FEC pointer usage in unique_ptr

struct FecDeleter {
    void operator()(fec_t *fecPtr) const {
        if (fecPtr) {
            fec_free(fecPtr);
        }
    }
};

//-------------------------------------------------------------
/**
 * @class Transmitter
 * @brief Base class providing FEC encoding, encryption, and session key management.
 *
 * Derived classes implement the actual injection (sending) of packets.
 */
class Transmitter {
  public:
    /**
     * @brief Constructs a Transmitter with specified FEC parameters, keypair file, epoch, and channel ID.
     * @param k Number of primary FEC fragments.
     * @param n Total FEC fragments.
     * @param keypair File path to keypair file.
     * @param epoch Unique epoch for the session.
     * @param channelId Channel identifier (e.g., linkId << 8 | radioPort).
     */
    Transmitter(int k, int n, const std::string &keypair, uint64_t epoch, uint32_t channelId);

    /**
     * @brief Virtual destructor to ensure proper cleanup in derived classes.
     */
    virtual ~Transmitter();

    /**
     * @brief Sends a single packet (optionally triggers FEC block finalization).
     * @param buf Pointer to payload data.
     * @param size Size in bytes of payload data.
     * @param flags Additional flags (e.g. WFB_PACKET_FEC_ONLY).
     * @return True if packet is enqueued or encoded, false if ignored (e.g. FEC-only while block is empty).
     */
    bool sendPacket(const uint8_t *buf, size_t size, uint8_t flags);

    /**
     * @brief Injects the current session key packet.
     */
    void sendSessionKey();

    /**
     * @brief Choose which output interface (antenna / socket / etc.) to use.
     * @param idx The interface index, or -1 for “mirror” mode.
     */
    virtual void selectOutput(int idx) = 0;

    /**
     * @brief Dumps statistics (injected vs. dropped packets, latencies, etc.) for derived transmitters.
     * @param fp File pointer to write stats.
     * @param ts Current timestamp (ms).
     * @param injectedPackets [out] cumulative count of injected packets.
     * @param droppedPackets [out] cumulative count of dropped packets.
     * @param injectedBytes [out] cumulative count of injected bytes.
     */
    virtual void
    dumpStats(FILE *fp, uint64_t ts, uint32_t &injectedPackets, uint32_t &droppedPackets, uint32_t &injectedBytes) = 0;

  protected:
    /**
     * @brief Actually injects (sends) the final buffer. Implemented by derived classes.
     * @param buf Pointer to data to send.
     * @param size Byte length of data to send.
     */
    virtual void injectPacket(const uint8_t *buf, size_t size) = 0;

  private:
    void sendBlockFragment(size_t packetSize);
    void makeSessionKey();

  private:
    // FEC encoding
    std::unique_ptr<fec_t, FecDeleter> fecPtr_;
    const unsigned short int fecK_;
    const unsigned short int fecN_;

    // Per-block counters
    uint64_t blockIndex_;
    uint8_t fragmentIndex_;
    std::vector<std::unique_ptr<uint8_t[]>> block_;
    size_t maxPacketSize_;

    // Session properties
    const uint64_t epoch_;
    const uint32_t channelId_;

    // Crypto keys
    uint8_t txSecretKey_[crypto_box_SECRETKEYBYTES];
    uint8_t rxPublicKey_[crypto_box_PUBLICKEYBYTES];
    uint8_t sessionKey_[crypto_aead_chacha20poly1305_KEYBYTES];

    // Session key packet buffer: header + data + Mac
    uint8_t sessionKeyPacket_[sizeof(wsession_hdr_t) + sizeof(wsession_data_t) + crypto_box_MACBYTES];
};

//-------------------------------------------------------------
/**
 * @class TxAntennaItem
 * @brief Tracks statistics for a single output interface/antenna.
 */
class TxAntennaItem {
  public:
    TxAntennaItem()
            : countPacketsInjected(0), countBytesInjected(0), countPacketsDropped(0), latencySum(0), latencyMin(0),
              latencyMax(0) {}

    /**
     * @brief Logs packet latency and updates injection/dropping stats.
     * @param latency Microseconds elapsed.
     * @param succeeded True if packet was sent successfully, false if dropped.
     * @param packetSize Number of bytes in the packet.
     */
    void logLatency(uint64_t latency, bool succeeded, uint32_t packetSize) {
        if ((countPacketsInjected + countPacketsDropped) == 0) {
            latencyMin = latency;
            latencyMax = latency;
        } else {
            latencyMin = std::min(latency, latencyMin);
            latencyMax = std::max(latency, latencyMax);
        }
        latencySum += latency;

        if (succeeded) {
            ++countPacketsInjected;
            countBytesInjected += packetSize;
        } else {
            ++countPacketsDropped;
        }
    }

    // Stats
    uint32_t countPacketsInjected;
    uint32_t countBytesInjected;
    uint32_t countPacketsDropped;
    uint64_t latencySum;
    uint64_t latencyMin;
    uint64_t latencyMax;
};

/// Map: key = (antennaIndex << 8) | 0xff, value = TxAntennaItem
using TxAntennaStat = std::unordered_map<uint64_t, TxAntennaItem>;

//-------------------------------------------------------------
/**
 * @class RawSocketTransmitter
 * @brief Transmitter that sends packets over raw AF_PACKET sockets.
 *
 * This transmitter can operate in single-output or “mirror” mode. In mirror mode,
 * it sends each packet through all raw sockets. In single-output mode, only one is selected.
 */
class RawSocketTransmitter : public Transmitter {
  public:
    RawSocketTransmitter(int k,
                         int n,
                         const std::string &keypair,
                         uint64_t epoch,
                         uint32_t channelId,
                         const std::vector<std::string> &wlans,
                         std::shared_ptr<uint8_t[]> radiotapHeader,
                         size_t radiotapHeaderLen,
                         uint8_t frameType);

    ~RawSocketTransmitter() override;

    void selectOutput(int idx) override { currentOutput_ = idx; }

    void dumpStats(
        FILE *fp, uint64_t ts, uint32_t &injectedPackets, uint32_t &droppedPackets, uint32_t &injectedBytes) override;

  private:
    void injectPacket(const uint8_t *buf, size_t size) override;

  private:
    const uint32_t channelId_;
    int currentOutput_;
    uint16_t ieee80211Sequence_;
    std::vector<int> sockFds_;
    TxAntennaStat antennaStat_;
    std::shared_ptr<uint8_t[]> radiotapHeader_;
    size_t radiotapHeaderLen_;
    uint8_t frameType_;
};

//-------------------------------------------------------------
/**
 * @class UdpTransmitter
 * @brief Transmitter that sends packets over a simple UDP socket.
 */
class UdpTransmitter : public Transmitter {
  public:
    UdpTransmitter(int k,
                   int n,
                   const std::string &keypair,
                   const std::string &clientAddr,
                   int basePort,
                   uint64_t epoch,
                   uint32_t channelId);

    ~UdpTransmitter() override;

    void dumpStats(FILE * /*fp*/,
                   uint64_t /*ts*/,
                   uint32_t & /*injectedPackets*/,
                   uint32_t & /*droppedPackets*/,
                   uint32_t & /*injectedBytes*/) override {
        // No stats for UDP in this example
    }

    void selectOutput(int idx) override;

  private:
    void injectPacket(const uint8_t *buf, size_t size) override;

  private:
    int sockFd_;
    int basePort_;
    struct sockaddr_in saddr_;
};

//-------------------------------------------------------------
/**
 * @class UsbTransmitter
 * @brief Transmitter that sends packets via an attached USB WiFi device (Rtl8812a).
 */
class UsbTransmitter : public Transmitter {
  public:
    UsbTransmitter(int k,
                   int n,
                   const std::string &keypair,
                   uint64_t epoch,
                   uint32_t channelId,
                   const std::vector<std::string> &wlans,
                   uint8_t *radiotapHeader,
                   size_t radiotapHeaderLen,
                   uint8_t frameType,
                   Rtl8812aDevice *device);

    ~UsbTransmitter() override = default;

    void selectOutput(int idx) override { currentOutput_ = idx; }

    void dumpStats(
        FILE *fp, uint64_t ts, uint32_t &injectedPackets, uint32_t &droppedPackets, uint32_t &injectedBytes) override;

  private:
    void injectPacket(const uint8_t *buf, size_t size) override;

  private:
    const uint32_t channelId_;
    int currentOutput_;
    uint16_t ieee80211Sequence_;
    TxAntennaStat antennaStat_;
    uint8_t *radiotapHeader_;
    size_t radiotapHeaderLen_;
    uint8_t frameType_;
    Rtl8812aDevice *rtlDevice_;
};

//-------------------------------------------------------------
/**
 * @struct TxArgs
 * @brief Command-line or user-provided arguments controlling the transmitter setup.
 */
struct TxArgs {
    uint8_t k = 8;
    uint8_t n = 12;
    uint8_t radio_port = 0;
    uint32_t link_id = 0x0;
    uint64_t epoch = 0;
    int udp_port = 5600;
    int log_interval = 1000;

    int bandwidth = 20;
    int short_gi = 0;
    int stbc = 0;
    int ldpc = 0;
    int mcs_index = 1;
    int vht_nss = 1;
    int debug_port = 0;
    int fec_timeout = 20;
    int rcv_buf = 0;
    bool mirror = false;
    bool vht_mode = false;
    std::string keypair = "tx.key";
};

//-------------------------------------------------------------
/**
 * @class TxFrame
 * @brief Orchestrates the receiving of inbound packets, the creation of Transmitter(s),
 *        and the forwarding of data via FEC and encryption.
 */
class TxFrame {
  public:
    TxFrame();
    ~TxFrame();

    /**
     * @brief Extracts the SO_RXQ_OVFL counter from msghdr control messages.
     * @param msg The msghdr structure from recvmsg.
     * @return The current overflow counter, or 0 if not present.
     */
    static uint32_t extractRxqOverflow(struct msghdr *msg);

    /**
     * @brief Main loop that polls inbound sockets, reading data and passing it to the transmitter.
     * @param transmitter The shared transmitter (UdpTransmitter, RawSocketTransmitter, etc.).
     * @param rxFds Vector of inbound sockets (e.g., from open_udp_socket_for_rx).
     * @param fecTimeout Timeout in ms for finalizing FEC blocks with empty packets.
     * @param mirror If true, sends the same packet to all outputs simultaneously.
     * @param logInterval Interval in ms for printing stats.
     */
    void dataSource(std::shared_ptr<Transmitter> &transmitter,
                    std::vector<int> &rxFds,
                    int fecTimeout,
                    bool mirror,
                    int logInterval);

    /**
     * @brief Configures and runs the transmitter with the given arguments.
     * @param rtlDevice The Rtl8812aDevice pointer (if using USB).
     * @param arg TxArgs structure with user parameters.
     */
    void run(Rtl8812aDevice *rtlDevice, TxArgs *arg);

    /**
     * @brief Signals that the main loop should stop.
     */
    void stop();

  private:
    bool shouldStop_ = false;
};
