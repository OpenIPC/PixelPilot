#ifndef PIXELPILOT_STA_LINK_H
#define PIXELPILOT_STA_LINK_H

// AP mode: devourer acts as a regular Wi-Fi station (client) and joins an OPEN
// access point, typically the camera itself. The station logic lives here in
// userspace - probe/auth/assoc, ARP, ICMP echo - on top of devourer's raw
// injection and monitor RX. The chip's hardware ACK responder is armed for our
// own MAC, so the AP sees every unicast frame to us acknowledged within SIFS.
//
// Data path: the camera pushes RTP/UDP to our static IP. UDP datagrams for the
// configured ports are handed to 127.0.0.1, where the video player (5600) and
// the MAVLink reader (14550) already listen in WFB-NG mode, so nothing above
// this layer needs to know which mode is active.
//
// Control path: one short TCP request/response at a time (tcpRequest), enough to
// talk to the camera's air_man_ap control port (e.g. "set_ap_channel 161").
//
// Out of scope: encryption (WEP/WPA), DHCP, IP fragments, general TCP.

#include "devourer/src/IRtlDevice.h"
#include "devourer/src/RxPacket.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct StaConfig {
    std::string ssid;
    std::array<uint8_t, 6> own_mac{};
    // IPv4 address in network byte order.
    std::array<uint8_t, 4> own_ip{};
    // Where to look first; channel switch announcements from the AP are followed.
    uint8_t channel{0};
    // Checked in turn when the SSID is not heard on `channel`, e.g. the channels
    // the camera passes through while booting.
    std::vector<uint8_t> candidate_channels;
    // After the candidates, also sweep every other channel.
    bool full_scan{false};
    // Widest channel the station may use (20, 40 or 80 MHz, 5 GHz only). It follows
    // the AP's width up to this; 20 is the safe fallback.
    int max_width_mhz{20};
    // UDP destination port on the air -> port on 127.0.0.1.
    std::vector<std::pair<uint16_t, uint16_t>> forward_ports;
};

class StaLink {
  public:
    enum class State { Searching, Authenticating, Associating, Connected };

    struct Stats {
        uint32_t rx_data;    // data frames from the AP addressed to us
        uint32_t rx_udp;     // UDP datagrams forwarded to localhost
        uint32_t rx_dropped; // data frames we could not use (fragments, encrypted, ...)
        uint32_t tx_frames;  // frames handed to the driver
    };

    StaLink(IRtlDevice *dev, StaConfig cfg);
    ~StaLink();

    void start();
    void stop();

    // Called from devourer's RX thread for every received frame.
    void onRx(const Packet &packet);

    State state() const { return state_.load(); }
    // Counters since the previous call.
    Stats takeStats();
    // Human readable one-liner for the UI.
    std::string statusText() const;
    // The channel the AP is on while connected, 0 otherwise.
    uint8_t connectedChannel() const;
    // Counts every time video can flow again: each (re)association and each end of a
    // dropout. The UI requests a key frame when it changes, so the decoder does not
    // wait for the next scheduled one.
    uint32_t recoveries() const { return recoveries_.load(); }

    // Queues a TX power change. It is applied by the worker thread, the only one that
    // may talk to the device's control path (retunes run there too).
    void requestTxPower(int power) { pending_tx_power_ = power; }

    // Opens a TCP connection to ip:port, sends `request` and collects the reply
    // until the peer closes, goes quiet after answering, or `timeout` passes.
    // half_close ends our sending side with the request (as `echo cmd | nc` does).
    // Blocks the caller (not an RX or worker thread). Waits for a request that is
    // already running; `timeout` covers that wait and the exchange together.
    // Returns false when not connected, the slot stayed busy, the reply is
    // missing, or an HTTP reply was cut off.
    bool tcpRequest(const std::array<uint8_t, 4> &ip,
                    uint16_t port,
                    const std::string &request,
                    std::chrono::milliseconds timeout,
                    bool half_close,
                    std::string &reply);

  private:
    using Clock = std::chrono::steady_clock;

    // Minimal TCP client: one connection, in-order receive only, retransmits on a timer.
    struct TcpSession {
        enum class St { Idle, Resolving, SynSent, Established, Done, Failed } st{St::Idle};
        std::array<uint8_t, 4> ip{};
        std::array<uint8_t, 6> mac{};
        uint16_t lport{0}, rport{0};
        uint32_t iss{0}, snd_nxt{0}, rcv_nxt{0};
        std::vector<uint8_t> data; // the request
        bool data_acked{false};
        bool half_close{false}; // FIN goes out with the request
        std::string rx;
        int retries{0};
        Clock::time_point last_tx{}, last_rx{}, deadline{};
    };
    void tcpTick(Clock::time_point now);
    void handleTcp(const uint8_t *ip, size_t ihl, size_t len);
    void tcpSend(uint8_t flags, uint32_t seq, const uint8_t *data, size_t len);
    void tcpFinish(bool ok);
    // Wraps an L4 payload in IPv4 and sends it to dst_mac.
    void sendIpv4(const uint8_t *dst_mac, const uint8_t *dst_ip, uint8_t proto, const std::vector<uint8_t> &l4);
    void learnMac(const uint8_t *ip, const uint8_t *mac);

    void worker();
    void tick();
    void setState(State s);
    // Authenticate to the known BSSID on the current channel right away - no search.
    void beginAuth();
    // The AP no longer has us associated (deauth, restart): rejoin it where it is.
    void reconnect(const char *why);
    // Once a second: frame rate, and while searching a log line listing what is on the air.
    void report(Clock::time_point now);

    void handleBeacon(const uint8_t *frame, size_t len, uint8_t rssi);
    // Channel Switch Announcement / Extended CSA elements from our AP.
    void handleCsa(const uint8_t *ies, size_t len);
    // The width to run at for the AP's HT/VHT operation elements (capped by the
    // config and devourer's limits); offset is devourer's 1 = HT40+, 2 = HT40-.
    int desiredWidth(const uint8_t *ies, size_t len, uint8_t &offset) const;
    // Queue a retune for the worker (which runs it outside mu_).
    void requestTune(int channel, int width_mhz, uint8_t offset);
    // Channels to scan, starting at home_channel_.
    void buildScanList();
    void handleAuth(const uint8_t *frame, size_t len);
    void handleAssocResp(const uint8_t *frame, size_t len);
    void handleData(const uint8_t *frame, size_t len, uint8_t rssi_a, uint8_t rssi_b);
    void handleMsdu(uint16_t ethertype, const uint8_t *payload, size_t len, const uint8_t *src_mac);
    void handleArp(const uint8_t *arp, size_t len);
    void handleIpv4(const uint8_t *ip, size_t len, const uint8_t *src_mac);

    void sendProbeRequest();
    void sendAuth();
    void sendAssocRequest();
    void sendArp(uint16_t oper, const uint8_t *target_mac, const uint8_t *target_ip, const uint8_t *dst_mac);
    void sendData(const uint8_t *dst_mac, uint16_t ethertype, const uint8_t *payload, size_t len);

    std::vector<uint8_t> mgmtHeader(uint8_t fc0, const uint8_t *addr1, const uint8_t *addr3);
    void appendRates(std::vector<uint8_t> &m) const;
    uint16_t nextSeq();
    void enqueue(std::vector<uint8_t> mpdu);

    IRtlDevice *dev_;
    const StaConfig cfg_;
    std::vector<uint8_t> radiotap_;
    int udp_sock_{-1};

    std::atomic<State> state_{State::Searching};
    std::atomic<bool> running_{false};
    std::thread worker_;

    // Protects everything below that the RX thread and the worker share.
    mutable std::mutex mu_;
    uint8_t channel_;                // what the radio is tuned to (the primary 20 MHz)
    int width_mhz_{20};              // tuned width
    uint8_t chan_offset_{0};         // tuned 40 MHz secondary: 0 none, 1 above, 2 below
    uint8_t home_channel_;           // where the AP was last found; scans start here
    std::vector<uint8_t> scan_list_; // scan_list_[0] == home_channel_
    size_t scan_index_{0};
    Clock::time_point dwell_start_{};
    int retune_to_{-1}; // set via requestTune(), carried out by the worker outside mu_
    int retune_width_{20};
    uint8_t retune_offset_{0};
    Clock::time_point last_width_change_{};
    bool ap_vht_{false};  // AP advertises VHT capabilities (needed for 80 MHz)
    int csa_channel_{-1}; // announced by the AP, switched to at csa_at_
    int csa_width_{20};   // ... at this width (from the CSA's width elements)
    uint8_t csa_offset_{0};
    Clock::time_point csa_at_{};
    std::array<uint8_t, 6> bssid_{};
    std::vector<uint8_t> ap_rates_; // AP's supported + extended rates (raw rate bytes)
    bool ap_ht_{false};
    bool ap_wmm_{false};
    bool warned_encrypted_{false};
    int attempts_{0};
    Clock::time_point last_tx_attempt_{};
    Clock::time_point last_heard_{};
    Clock::time_point last_garp_{};
    uint16_t aid_{0};
    int last_rx_seq_{-1};
    uint8_t last_rssi_{0}; // raw chip value, same scale wfb-ng mode feeds the signal-quality calculator
    bool in_gap_{false};   // connected, but nothing heard from the AP for kDropoutGap
    uint64_t last_tsf_{0}; // AP's beacon timestamp; jumping back means the AP restarted
    std::atomic<uint32_t> recoveries_{0};
    std::atomic<int> pending_tx_power_{-1}; // -1: nothing queued

    // Diagnostics, so a user can tell why nothing connects.
    struct SeenNetwork {
        std::string ssid;
        int channel;
        bool encrypted;
        uint8_t rssi;
        Clock::time_point last;
    };
    std::map<std::array<uint8_t, 6>, SeenNetwork> seen_; // by BSSID, while searching
    Clock::time_point last_report_{};
    Clock::time_point last_seen_log_{};
    uint32_t frames_at_last_report_{0};
    uint32_t frames_per_sec_{0};
    std::set<uint16_t> warned_ports_;       // UDP ports for us that nothing listens on
    std::set<uint32_t> warned_arp_targets_; // IPs the camera looks for that are not ours
    bool have_camera_ip_{false};
    std::array<uint8_t, 4> camera_ip_{};
    std::atomic<uint32_t> rx_frames_total_{0};

    // IPv4 reassembly: a few datagrams in flight, keyed by source + IP id.
    struct IpFragments {
        uint32_t src;
        uint16_t id;
        std::vector<uint8_t> header; // from the offset-0 fragment
        std::vector<uint8_t> payload;
        std::vector<bool> got; // one flag per 8-byte unit of payload that has arrived
        size_t total{0};       // known once the last fragment arrived
        Clock::time_point start;
    };
    std::vector<IpFragments> frags_;
    // Returns the complete datagram once all fragments are in, otherwise empty.
    std::vector<uint8_t> reassemble(const uint8_t *ip, size_t ihl, size_t len);

    // Per-5s traffic summary while connected (see report()).
    std::map<uint16_t, uint32_t> udp_by_port_; // UDP datagrams for us, by destination port
    uint32_t sum_data_{0}, sum_fragments_{0}, sum_reassembled_{0}, sum_protected_{0}, sum_not_ipv4_{0},
        sum_mac_fragments_{0};
    bool warned_mac_fragments_{false};
    Clock::time_point last_traffic_log_{};
    std::set<uint16_t> seen_udp_ports_; // first datagram per port is logged with its source

    std::map<uint32_t, std::array<uint8_t, 6>> arp_cache_; // IPv4 (as read from the wire) -> MAC
    TcpSession tcp_;
    std::condition_variable tcp_cv_; // waits on mu_
    uint16_t ip_id_{0};

    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::vector<std::vector<uint8_t>> tx_queue_;
    uint16_t seq_{0};

    std::atomic<uint32_t> rx_data_{0};
    std::atomic<uint32_t> rx_udp_{0};
    std::atomic<uint32_t> rx_dropped_{0};
    std::atomic<uint32_t> tx_frames_{0};
};

#endif // PIXELPILOT_STA_LINK_H
