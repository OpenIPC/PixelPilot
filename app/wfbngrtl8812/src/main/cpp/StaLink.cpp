#include "StaLink.h"

#include "SignalQualityCalculator.h"
#include "devourer/src/RadiotapBuilder.h"

#include <algorithm>
#include <android/log.h>
#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <netinet/in.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

#undef TAG
#define TAG "pixelpilot-sta"

using namespace std::chrono_literals;

namespace {

constexpr uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr uint8_t kZeroMac[6] = {0, 0, 0, 0, 0, 0};
constexpr uint8_t kLlcSnap[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};

// Management frame retransmission. On Jaguar1 (RTL8812AU) devourer marks every
// injected frame as group-addressed, so the chip never retries in hardware and
// these software retries are all there is.
constexpr auto kMgmtRetryInterval = 300ms;
constexpr int kMgmtMaxAttempts = 5;
constexpr auto kProbeInterval = 500ms;
constexpr auto kTickInterval = 50ms;
// Silence does NOT end the association: the AP keeps us associated through a
// fade, and video must be taken the moment it is heard again. The association is
// only redone when the AP says so (deauth/disassoc, or a deauth in answer to our
// keepalive after it restarted) or its beacon timestamp shows a restart.
//
// A silence this long counts as a dropout (beacons come every ~102 ms). When the
// AP is heard again, a keepalive goes out at once to check the association, and
// the UI asks the camera for a key frame.
constexpr auto kDropoutGap = 250ms;
// After this long without the AP, also peek at the candidate channels in case the
// camera rebooted onto one of its boot channels. Most of the time stays on the
// home channel, where a camera that only faded comes back.
constexpr auto kPeekAfter = 3s;
constexpr auto kPeekHomeDwell = 1000ms;
constexpr auto kPeekDwell = 150ms;
// A beacon timestamp this far behind the previous one means the AP restarted.
constexpr uint64_t kTsfRestartUs = 1000000;
// Re-announce our IP so the camera's ARP cache never expires mid-flight.
constexpr auto kGarpInterval = 30s;
// Null-data keepalive. The camera's hostapd polls a station after 20 s of
// silence (ap_max_inactivity=20) and drops it on missing ACKs; regular frames
// from us keep it from ever getting there.
constexpr auto kKeepaliveInterval = 5s;
// Channel scan while searching: a long look at the home channel, then a short
// one per other channel. 200 ms catches at least one beacon (every ~102 ms) and
// the answer to the probe request sent on arrival.
constexpr auto kHomeDwell = 1500ms;
constexpr auto kScanDwell = 200ms;
// 802.11 time unit; beacon intervals and CSA counts are in TU.
constexpr auto kTu = std::chrono::microseconds(1024);

// OpenIPC APs use UNII-3 or UNII-1, so those go first; DFS and 2.4 GHz last.
constexpr uint8_t kScanChannels[] = {149, 153, 157, 161, 165, 36,  40,  44,  48,  52,  56,  60,  64,
                                     100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 1,
                                     2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13};

constexpr uint16_t kEthIpv4 = 0x0800;
constexpr uint16_t kEthArp = 0x0806;

bool macEq(const uint8_t *a, const uint8_t *b) { return std::memcmp(a, b, 6) == 0; }

// One's-complement sum over big-endian 16-bit words, for IP/ICMP/TCP checksums.
uint32_t csumAdd(uint32_t s, const uint8_t *d, size_t len) {
    for (size_t i = 0; i + 1 < len; i += 2)
        s += (uint32_t)(d[i] << 8) | d[i + 1];
    if (len & 1) s += (uint32_t)d[len - 1] << 8;
    return s;
}

uint16_t csumFold(uint32_t s) {
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

uint16_t csum16(const uint8_t *d, size_t len) { return csumFold(csumAdd(0, d, len)); }

uint32_t ipKey(const uint8_t *ip) {
    uint32_t k;
    std::memcpy(&k, ip, 4);
    return k;
}

uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void wr32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

constexpr uint8_t kTcpFin = 0x01, kTcpSyn = 0x02, kTcpRst = 0x04, kTcpPsh = 0x08, kTcpAck = 0x10;
constexpr auto kTcpRetransmit = 400ms;
// air_man_ap answers once it is done; when it keeps the connection open after
// that, this much silence after the first reply bytes ends the request.
constexpr auto kTcpReplyIdle = 1500ms;
// Replies are small status lines or JSON; anything beyond this is not from the camera.
constexpr size_t kTcpMaxReply = 64 * 1024;
// ICMP echo requests larger than a normal MTU are not answered (they would have to go
// out as one oversized MPDU).
constexpr size_t kMaxEchoDatagram = 1500;
// Networks remembered while searching (diagnostics only).
constexpr size_t kMaxSeenNetworks = 64;

// True for an HTTP reply whose headers or body are not all there yet. Anything that
// is not HTTP (air_man_ap speaks plain lines) counts as complete.
bool httpReplyIncomplete(const std::string &rx) {
    static const std::string kHttp = "HTTP/";
    // Fewer bytes than the status line prefix: still HTTP if they match its start.
    if (rx.size() < kHttp.size()) return !rx.empty() && kHttp.compare(0, rx.size(), rx) == 0;
    if (rx.compare(0, kHttp.size(), kHttp) != 0) return false;
    const auto hdr_end = rx.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return true;
    std::string head = rx.substr(0, hdr_end);
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char ch) { return std::tolower(ch); });
    const auto cl = head.find("\r\ncontent-length:");
    if (cl == std::string::npos) return false; // delimited by the peer closing
    const size_t want = std::strtoul(head.c_str() + cl + 17, nullptr, 10);
    return rx.size() - (hdr_end + 4) < want;
}

const char *stateName(StaLink::State s) {
    switch (s) {
    case StaLink::State::Searching:
        return "searching";
    case StaLink::State::Authenticating:
        return "authenticating";
    case StaLink::State::Associating:
        return "associating";
    case StaLink::State::Connected:
        return "connected";
    }
    return "?";
}

} // namespace

StaLink::StaLink(IRtlDevice *dev, StaConfig cfg)
        : dev_(dev), cfg_(std::move(cfg)), channel_(cfg_.channel), home_channel_(cfg_.channel) {
    buildScanList();
    radiotap_ = devourer::build_stream_radiotap(devourer::parse_tx_mode_str("6M"));
    udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock_ < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "could not create the forwarding socket");
    }
}

StaLink::~StaLink() {
    stop();
    if (udp_sock_ >= 0) close(udp_sock_);
}

void StaLink::start() {
    if (running_.exchange(true)) return;
    __android_log_print(ANDROID_LOG_INFO,
                        TAG,
                        "joining open network \"%s\" on channel %d as %02x:%02x:%02x:%02x:%02x:%02x / %d.%d.%d.%d",
                        cfg_.ssid.c_str(),
                        cfg_.channel,
                        cfg_.own_mac[0],
                        cfg_.own_mac[1],
                        cfg_.own_mac[2],
                        cfg_.own_mac[3],
                        cfg_.own_mac[4],
                        cfg_.own_mac[5],
                        cfg_.own_ip[0],
                        cfg_.own_ip[1],
                        cfg_.own_ip[2],
                        cfg_.own_ip[3]);
    {
        // First "networks heard" report only after a full listening window.
        std::lock_guard<std::mutex> lock(mu_);
        last_report_ = last_seen_log_ = dwell_start_ = Clock::now();
    }
    worker_ = std::thread([this] { worker(); });
}

void StaLink::stop() {
    if (!running_.exchange(false)) return;
    q_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    // Release a caller blocked in tcpRequest(); nothing will answer it any more.
    std::lock_guard<std::mutex> lock(mu_);
    tcpFinish(false);
    tcp_cv_.notify_all(); // callers queued for the session slot give up too
}

uint8_t StaLink::connectedChannel() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_.load() == State::Connected ? channel_ : 0;
}

StaLink::Stats StaLink::takeStats() {
    return Stats{rx_data_.exchange(0), rx_udp_.exchange(0), rx_dropped_.exchange(0), tx_frames_.exchange(0)};
}

std::string StaLink::statusText() const {
    std::lock_guard<std::mutex> lock(mu_);
    char buf[160];
    State s = state_.load();
    if (s == State::Searching) {
        // What the adapter hears tells apart "wrong channel/SSID" from "adapter deaf".
        std::string heard;
        int n = 0;
        for (const auto &[bssid, net] : seen_) {
            if (n++ == 3) {
                heard += ", ...";
                break;
            }
            if (!heard.empty()) heard += ", ";
            heard += net.ssid.empty() ? "<hidden>" : net.ssid;
            if (net.encrypted) heard += " (encrypted)";
        }
        snprintf(buf,
                 sizeof(buf),
                 "APFPV: searching \"%s\" ch %d | %u frames/s | heard: %s",
                 cfg_.ssid.c_str(),
                 channel_,
                 frames_per_sec_,
                 heard.empty() ? "no networks" : heard.c_str());
    } else {
        snprintf(buf,
                 sizeof(buf),
                 "APFPV: %s \"%s\" ch %d/%dMHz %02x:%02x:%02x:%02x:%02x:%02x rssi %u",
                 stateName(s),
                 cfg_.ssid.c_str(),
                 channel_,
                 width_mhz_,
                 bssid_[0],
                 bssid_[1],
                 bssid_[2],
                 bssid_[3],
                 bssid_[4],
                 bssid_[5],
                 last_rssi_);
        std::string text = buf;
        const auto silence = Clock::now() - last_heard_;
        if (s == State::Connected && silence >= kDropoutGap) {
            snprintf(buf, sizeof(buf), " | no signal %.1f s", std::chrono::duration<double>(silence).count());
            text += buf;
        }
        if (have_camera_ip_) {
            snprintf(buf,
                     sizeof(buf),
                     " | video from %d.%d.%d.%d",
                     camera_ip_[0],
                     camera_ip_[1],
                     camera_ip_[2],
                     camera_ip_[3]);
            text += buf;
        }
        return text;
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Worker: owns all transmissions. devourer's send_packet() must not be called
// from the RX callback (libusb answers BUSY there), so the RX side only queues.
// ---------------------------------------------------------------------------

void StaLink::worker() {
    auto last_tick = Clock::time_point{};
    while (running_) {
        int retune = -1;
        int retune_width = 20, cur_width = 20;
        uint8_t retune_offset = 0, cur_offset = 0;
        auto now = Clock::now();
        if (now - last_tick >= kTickInterval) {
            last_tick = now;
            std::lock_guard<std::mutex> lock(mu_);
            tick();
            retune = std::exchange(retune_to_, -1);
            retune_width = retune_width_;
            retune_offset = retune_offset_;
            cur_width = width_mhz_;
            cur_offset = chan_offset_;
        }

        std::vector<std::vector<uint8_t>> batch;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            if (retune < 0) q_cv_.wait_for(lk, 20ms, [this] { return !tx_queue_.empty() || !running_; });
            batch.swap(tx_queue_);
        }
        for (auto &f : batch) {
            if (dev_->send_packet(f.data(), f.size())) tx_frames_++;
        }

        // Control-path calls stay on this thread (see IRtlDevice: single control thread).
        if (const int power = pending_tx_power_.exchange(-1); power >= 0) {
            try {
                dev_->SetTxPower(power);
            } catch (const std::exception &e) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "SetTxPower(%d) failed: %s", power, e.what());
            }
        }

        if (retune > 0) {
            bool tuned = true;
            // Outside mu_: a retune takes milliseconds (more on a band change) and the
            // RX thread must keep draining meanwhile. devourer's own hopping RX does the
            // same from a second thread while StartRxLoop runs.
            try {
                if (retune_width == cur_width && retune_offset == cur_offset) {
                    // Same width: the lean channel-only switch (~1-2 ms).
                    dev_->FastRetune(static_cast<uint8_t>(retune), true);
                } else {
                    // Width change: the full channel set (~90 ms), which FastRetune can't do.
                    dev_->SetMonitorChannel(SelectedChannel{
                        .Channel = static_cast<uint8_t>(retune),
                        .ChannelOffset = retune_offset,
                        .ChannelWidth = retune_width == 80   ? CHANNEL_WIDTH_80
                                        : retune_width == 40 ? CHANNEL_WIDTH_40
                                                             : CHANNEL_WIDTH_20,
                    });
                    __android_log_print(ANDROID_LOG_INFO,
                                        TAG,
                                        "tuned ch %d at %d MHz%s",
                                        retune,
                                        retune_width,
                                        retune_width == 40 ? (retune_offset == 2 ? " (HT40-)" : " (HT40+)") : "");
                }
            } catch (const std::exception &e) {
                tuned = false;
                __android_log_print(ANDROID_LOG_ERROR, TAG, "retune to ch %d failed: %s", retune, e.what());
            }
            std::lock_guard<std::mutex> lock(mu_);
            // Only record what the radio really switched to; after a failure the old
            // channel/width stay in force, so the search and probe logic keeps working
            // from the truth and simply tries again after the dwell time.
            if (tuned) {
                channel_ = static_cast<uint8_t>(retune);
                width_mhz_ = retune_width;
                chan_offset_ = retune_offset;
            }
            dwell_start_ = Clock::now();
            last_tx_attempt_ = Clock::time_point{}; // probe right away on the new channel
        }
    }
}

void StaLink::setState(State s) {
    State old = state_.exchange(s);
    if (old == s) return;
    __android_log_print(ANDROID_LOG_INFO, TAG, "state %s -> %s", stateName(old), stateName(s));
    attempts_ = 0;
    last_tx_attempt_ = Clock::time_point{};
    if (old == State::Connected) {
        tcpFinish(false); // a request in flight would only time out now
    }
    if (s == State::Connected) {
        in_gap_ = false;
        recoveries_++;
    }
    if (s == State::Searching) {
        aid_ = 0;
        last_rx_seq_ = -1;
        csa_channel_ = -1;
        last_tsf_ = 0;
        // Look where the AP was first, for a full home dwell, before scanning.
        dwell_start_ = Clock::now();
        // Searching runs at 20 MHz: beacons are on the primary 20, and a wide
        // tuning on a scan channel would centre the radio off it.
        if (channel_ != home_channel_ || width_mhz_ != 20) {
            requestTune(home_channel_, 20, 0);
        }
        scan_index_ = 0;
    }
}

void StaLink::requestTune(int channel, int width_mhz, uint8_t offset) {
    retune_to_ = channel;
    retune_width_ = width_mhz;
    retune_offset_ = offset;
}

int StaLink::desiredWidth(const uint8_t *ies, size_t len, uint8_t &offset) const {
    offset = 0;
    if (cfg_.max_width_mhz < 40 || channel_ <= 14) return 20;
    int primary = -1, sec = 0, vht_width = 0, vht_center = 0;
    bool any_width = false;
    for (size_t i = 0; i + 2 <= len;) {
        const uint8_t id = ies[i];
        const uint8_t l = ies[i + 1];
        const uint8_t *v = ies + i + 2;
        if (i + 2 + l > len) break;
        if (id == 61 && l >= 2) { // HT Operation: primary, secondary offset + STA width bits
            primary = v[0];
            sec = v[1] & 0x03;
            any_width = (v[1] & 0x04) != 0;
        } else if (id == 192 && l >= 3) { // VHT Operation: width, centre segment 0
            vht_width = v[0];
            vht_center = v[1];
        }
        i += 2 + l;
    }
    if (primary != channel_ || !any_width || (sec != 1 && sec != 3)) return 20;
    offset = sec == 1 ? 1 : 2;
    // devourer tunes 80 MHz only with the primary as the lowest 20 of the block
    // (centre = primary + 6), which every 80 MHz channel air_man_ap offers is.
    if (vht_width == 1 && cfg_.max_width_mhz >= 80 && ap_vht_ && vht_center == channel_ + 6) {
        offset = 0;
        return 80;
    }
    return 40;
}

void StaLink::beginAuth() {
    setState(State::Authenticating);
    attempts_ = 1;
    last_tx_attempt_ = Clock::now();
    sendAuth();
}

void StaLink::reconnect(const char *why) {
    __android_log_print(ANDROID_LOG_WARN,
                        TAG,
                        "%s: rejoining %02x:%02x:%02x:%02x:%02x:%02x on ch %d",
                        why,
                        bssid_[0],
                        bssid_[1],
                        bssid_[2],
                        bssid_[3],
                        bssid_[4],
                        bssid_[5],
                        channel_);
    aid_ = 0;
    last_rx_seq_ = -1;
    beginAuth();
}

void StaLink::buildScanList() {
    scan_list_.clear();
    auto add = [this](uint8_t ch) {
        if (ch != 0 && std::find(scan_list_.begin(), scan_list_.end(), ch) == scan_list_.end()) {
            scan_list_.push_back(ch);
        }
    };
    add(home_channel_);
    for (uint8_t ch : cfg_.candidate_channels)
        add(ch);
    if (cfg_.full_scan) {
        for (uint8_t ch : kScanChannels)
            add(ch);
    }
    scan_index_ = 0;
}

// Called with mu_ held. `ies` points at the information elements of a beacon,
// probe response or CSA action frame from our AP.
void StaLink::handleCsa(const uint8_t *ies, size_t len) {
    int new_channel = -1;
    int count = 0;
    int op_class = 0;
    int sec = -1;      // Secondary Channel Offset element: 0 none, 1 above, 3 below
    int wb_width = -1; // Wide Bandwidth Channel Switch: 0 = 20/40, 1 = 80
    int wb_center = 0;
    auto wide_bw = [&](const uint8_t *v, uint8_t l) {
        if (l >= 2) {
            wb_width = v[0];
            wb_center = v[1];
        }
    };
    for (size_t i = 0; i + 2 <= len;) {
        const uint8_t id = ies[i];
        const uint8_t l = ies[i + 1];
        const uint8_t *v = ies + i + 2;
        if (i + 2 + l > len) break;
        if (id == 37 && l >= 3) { // Channel Switch Announcement: mode, channel, count
            new_channel = v[1];
            count = v[2];
        } else if (id == 60 && l >= 4) { // Extended CSA: mode, operating class, channel, count
            op_class = v[1];
            new_channel = v[2];
            count = v[3];
        } else if (id == 62 && l >= 1) {
            sec = v[0];
        } else if (id == 194) {
            wide_bw(v, l);
        } else if (id == 196) { // Channel Switch Wrapper: sub-elements, 194 among them
            for (size_t j = 0; j + 2 <= l;) {
                const uint8_t sl = v[j + 1];
                if (j + 2 + sl > l) break;
                if (v[j] == 194) wide_bw(v + j + 2, sl);
                j += 2 + sl;
            }
        }
        i += 2 + l;
    }
    if (new_channel <= 0 || new_channel == csa_channel_) return;

    // The new width, so the radio lands on it right at the switch: air_man_ap pings us
    // straight after a switch and reverts when that fails, and a station still at
    // 20 MHz cannot decode a 40/80 MHz ping.
    int width = 20;
    uint8_t offset = 0;
    if (sec == 1 || sec == 3) {
        width = 40;
        offset = sec == 1 ? 1 : 2;
    } else if (sec < 0) {
        // Global operating classes (802.11 Annex E): 40 MHz above / below, 80 MHz.
        if (op_class == 116 || op_class == 119 || op_class == 122 || op_class == 126) {
            width = 40;
            offset = 1;
        } else if (op_class == 117 || op_class == 120 || op_class == 123 || op_class == 127) {
            width = 40;
            offset = 2;
        } else if (op_class == 128) {
            width = 80;
            wb_width = 1;
            wb_center = new_channel + 6; // only usable for us with the primary lowest anyway
        }
    }
    if (wb_width == 1 && (width == 40 || width == 80)) {
        if (wb_center == new_channel + 6) {
            width = 80;
            offset = 0;
        } else if (width == 80) {
            width = 20; // primary not the lowest 20: devourer can't tune it
        }
    }
    if (new_channel <= 14 || cfg_.max_width_mhz < 40) {
        width = 20;
        offset = 0;
    } else if (width == 80 && (cfg_.max_width_mhz < 80 || !ap_vht_)) {
        width = offset ? 40 : 20;
    }
    if (new_channel == channel_ && width == width_mhz_ && offset == chan_offset_) return;

    // count = beacons until the switch (0: any time now). Leave one beacon early
    // rather than late: being away briefly costs less than missing the switch.
    csa_channel_ = new_channel;
    csa_width_ = width;
    csa_offset_ = offset;
    csa_at_ = Clock::now() + std::max(count - 1, 0) * 100 * kTu;
    __android_log_print(ANDROID_LOG_INFO,
                        TAG,
                        "AP announces a switch to ch %d (%d MHz for us) in %d beacons",
                        new_channel,
                        width,
                        count);
}

// Called with mu_ held.
void StaLink::tick() {
    auto now = Clock::now();
    report(now);

    if (csa_channel_ > 0 && state_.load() != State::Searching && now >= csa_at_) {
        // The AP announced a channel switch and is leaving now; go with it. The
        // association survives a CSA, so the state stays as it is.
        __android_log_print(
            ANDROID_LOG_INFO, TAG, "following the AP: ch %d -> ch %d at %d MHz", channel_, csa_channel_, csa_width_);
        requestTune(csa_channel_, csa_width_, csa_offset_);
        last_width_change_ = now; // the beacon check would otherwise second-guess it mid-switch
        home_channel_ = static_cast<uint8_t>(csa_channel_);
        buildScanList();
        csa_channel_ = -1;
        last_heard_ = now; // no beacons while switching
        return;
    }

    switch (state_.load()) {
    case State::Searching: {
        // Not heard on this channel within the dwell: move on through the scan list.
        const auto dwell = channel_ == home_channel_ ? kHomeDwell : kScanDwell;
        if (scan_list_.size() > 1 && now - dwell_start_ >= dwell) {
            scan_index_ = (scan_index_ + 1) % scan_list_.size();
            requestTune(scan_list_[scan_index_], 20, 0);
            break;
        }
        // DFS channels are passive-scan only: no transmitting until an AP is heard there.
        const bool dfs = channel_ >= 52 && channel_ <= 144;
        if (!dfs && now - last_tx_attempt_ >= kProbeInterval) {
            last_tx_attempt_ = now;
            sendProbeRequest();
        }
        break;
    }
    case State::Authenticating:
    case State::Associating:
        if (now - last_tx_attempt_ >= kMgmtRetryInterval) {
            if (attempts_ >= kMgmtMaxAttempts) {
                __android_log_print(ANDROID_LOG_WARN, TAG, "no answer from the AP, searching again");
                setState(State::Searching);
                break;
            }
            attempts_++;
            last_tx_attempt_ = now;
            if (state_.load() == State::Authenticating) {
                sendAuth();
            } else {
                sendAssocRequest();
            }
        }
        break;
    case State::Connected: {
        const auto silence = now - last_heard_;
        if (silence >= kDropoutGap) in_gap_ = true; // the end of it is handled in onRx()
        if (silence >= kPeekAfter && scan_list_.size() > 1) {
            // Long outage: listen at home, glance at one candidate channel in between.
            if (channel_ == home_channel_) {
                if (now - dwell_start_ >= kPeekHomeDwell) {
                    scan_index_ = scan_index_ % (scan_list_.size() - 1) + 1;
                    requestTune(scan_list_[scan_index_], 20, 0);
                }
            } else if (now - dwell_start_ >= kPeekDwell) {
                requestTune(home_channel_, 20, 0);
            }
            break;
        }
        if (now - last_garp_ >= kGarpInterval) {
            last_garp_ = now;
            sendArp(1, kZeroMac, cfg_.own_ip.data(), kBroadcast);
        }
        if (now - last_tx_attempt_ >= kKeepaliveInterval) {
            last_tx_attempt_ = now;
            // Null function (data, no payload), ToDS, power management bit clear.
            std::vector<uint8_t> m = mgmtHeader(0x48, bssid_.data(), bssid_.data());
            m[1] = 0x01;
            enqueue(std::move(m));
        }
        tcpTick(now);
        break;
    }
    }
}

// Called with mu_ held.
void StaLink::report(Clock::time_point now) {
    if (now - last_report_ < 1s) return;
    const uint32_t total = rx_frames_total_.load();
    frames_per_sec_ = total - frames_at_last_report_;
    frames_at_last_report_ = total;
    last_report_ = now;

    for (auto it = seen_.begin(); it != seen_.end();) {
        it = (now - it->second.last > 10s) ? seen_.erase(it) : std::next(it);
    }

    if (state_.load() == State::Connected && now - last_traffic_log_ >= 5s) {
        // What arrives from the AP and where it goes: tells "camera sends nothing" from
        // "camera sends something we drop" without a capture.
        last_traffic_log_ = now;
        std::string ports;
        for (const auto &[port, n] : udp_by_port_) {
            bool fwd = false;
            for (const auto &f : cfg_.forward_ports)
                fwd |= f.first == port;
            char b[48];
            snprintf(b, sizeof(b), " %u:%u%s", port, n, fwd ? "" : "(not forwarded)");
            ports += b;
        }
        __android_log_print(ANDROID_LOG_INFO,
                            TAG,
                            "last 5 s on ch %d: %u data frames, UDP for us by port:%s | IP fragments %u "
                            "(reassembled %u), 802.11 fragments %u, protected %u, non-IPv4 %u",
                            channel_,
                            sum_data_,
                            ports.empty() ? " none" : ports.c_str(),
                            sum_fragments_,
                            sum_reassembled_,
                            sum_mac_fragments_,
                            sum_protected_,
                            sum_not_ipv4_);
        udp_by_port_.clear();
        sum_data_ = sum_fragments_ = sum_reassembled_ = sum_protected_ = sum_not_ipv4_ = sum_mac_fragments_ = 0;
    }

    if (state_.load() != State::Searching || now - last_seen_log_ < 5s) return;
    last_seen_log_ = now;
    if (seen_.empty()) {
        __android_log_print(ANDROID_LOG_WARN,
                            TAG,
                            "searching \"%s\" on ch %d: %u frames/s received, no beacons heard. "
                            "Wrong channel, or the camera's AP is not up?",
                            cfg_.ssid.c_str(),
                            channel_,
                            frames_per_sec_);
        return;
    }
    __android_log_print(ANDROID_LOG_WARN,
                        TAG,
                        "searching \"%s\" on ch %d: %u frames/s received, networks heard:",
                        cfg_.ssid.c_str(),
                        channel_,
                        frames_per_sec_);
    for (const auto &[b, net] : seen_) {
        __android_log_print(ANDROID_LOG_WARN,
                            TAG,
                            "  \"%s\" %02x:%02x:%02x:%02x:%02x:%02x ch %d %s rssi %u",
                            net.ssid.c_str(),
                            b[0],
                            b[1],
                            b[2],
                            b[3],
                            b[4],
                            b[5],
                            net.channel,
                            net.encrypted ? "encrypted" : "open",
                            net.rssi);
    }
}

// ---------------------------------------------------------------------------
// RX
// ---------------------------------------------------------------------------

void StaLink::onRx(const Packet &packet) {
    if (packet.RxAtrib.crc_err || packet.Data.size() < 24 + 4) return;
    rx_frames_total_++;
    const uint8_t *f = packet.Data.data();
    const size_t len = packet.Data.size() - 4; // strip FCS

    const uint8_t fc0 = f[0];
    const uint8_t type = (fc0 >> 2) & 0x03;
    const uint8_t *addr1 = f + 4;
    const uint8_t *addr2 = f + 10;

    std::lock_guard<std::mutex> lock(mu_);
    const bool from_ap = state_.load() != State::Searching && macEq(addr2, bssid_.data());
    if (from_ap) {
        const auto now = Clock::now();
        // tick() flags a gap only every kTickInterval; judge the silence here too, so a
        // dropout that ends between two ticks still triggers the key-frame request.
        if (state_.load() == State::Connected && last_heard_ != Clock::time_point{} &&
            (in_gap_ || now - last_heard_ >= kDropoutGap)) {
            // Dropout over: video flows again right now, nothing to rebuild.
            in_gap_ = false;
            recoveries_++;
            __android_log_print(
                ANDROID_LOG_INFO,
                TAG,
                "AP back after %lld ms on ch %d",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heard_).count(),
                channel_);
            if (channel_ != home_channel_) {
                // Heard while peeking at a candidate: the camera lives here now.
                home_channel_ = channel_;
                buildScanList();
            }
            // Keepalive right away: an AP that dropped us (or restarted) answers it with a
            // deauth, which rejoins at once instead of after the next keepalive.
            last_tx_attempt_ = Clock::time_point{};
        }
        last_heard_ = now;
        last_rssi_ = packet.RxAtrib.rssi[0];
    }

    if (type == 0) { // management
        const bool to_us = macEq(addr1, cfg_.own_mac.data());
        switch (fc0) {
        case 0x80: // beacon
        case 0x50: // probe response
            if (state_.load() == State::Searching) {
                handleBeacon(f, len, packet.RxAtrib.rssi[0]);
            } else if (from_ap && len > 36) {
                uint64_t tsf = 0;
                for (int i = 7; i >= 0; i--)
                    tsf = (tsf << 8) | f[24 + i];
                if (state_.load() == State::Connected && last_tsf_ != 0 && tsf + kTsfRestartUs < last_tsf_) {
                    // A restarted hostapd no longer knows us; don't wait for its deauth.
                    last_tsf_ = tsf;
                    reconnect("AP restarted (beacon timestamp went back)");
                    break;
                }
                last_tsf_ = tsf;
                handleCsa(f + 36, len - 36);
                // Match the AP's width (within our limits). Covers joining, a CSA to a
                // different width (followed at 20 MHz first) and a return from a peek.
                if (state_.load() == State::Connected && retune_to_ < 0 && csa_channel_ < 0) {
                    uint8_t off;
                    const int w = desiredWidth(f + 36, len - 36, off);
                    const auto now = Clock::now();
                    if ((w != width_mhz_ || off != chan_offset_) && now - last_width_change_ > 1s) {
                        last_width_change_ = now;
                        __android_log_print(ANDROID_LOG_INFO,
                                            TAG,
                                            "AP runs ch %d at a different width: %d -> %d MHz",
                                            channel_,
                                            width_mhz_,
                                            w);
                        requestTune(channel_, w, off);
                    }
                }
            }
            break;
        case 0xb0: // authentication
            if (from_ap && to_us) handleAuth(f, len);
            break;
        case 0x10: // association response
        case 0x30: // reassociation response
            if (from_ap && to_us) handleAssocResp(f, len);
            break;
        case 0xa0: // disassociation
        case 0xc0: // deauthentication
            if (from_ap && (to_us || (addr1[0] & 0x01))) {
                uint16_t reason = len >= 26 ? (f[24] | (f[25] << 8)) : 0;
                char why[64];
                snprintf(why,
                         sizeof(why),
                         "%s by the AP, reason %u",
                         fc0 == 0xa0 ? "disassociated" : "deauthenticated",
                         reason);
                if (state_.load() == State::Connected) {
                    // The AP is right here, so rejoin it directly instead of searching.
                    reconnect(why);
                } else {
                    // Mid-join: the bounded auth/assoc retries in tick() handle it, so an
                    // AP that keeps refusing cannot drive a tight reconnect loop.
                    __android_log_print(ANDROID_LOG_WARN, TAG, "%s while joining", why);
                }
            }
            break;
        case 0xd0: // action
            if (from_ap && len >= 24 + 6 && f[25] == 4) {
                if (f[24] == 0) { // spectrum management: CSA element follows
                    handleCsa(f + 26, len - 26);
                } else if (f[24] == 4) { // public: extended CSA, fields without element header
                    const uint8_t ecsa[] = {60, 4, f[26], f[27], f[28], f[29]};
                    handleCsa(ecsa, sizeof(ecsa));
                }
            }
            // Decline block-ack sessions (ADDBA request -> response "request declined"),
            // so the AP stops asking and sends plain MPDUs.
            if (from_ap && to_us && len >= 24 + 9 && f[24] == 3 && f[25] == 0) {
                std::vector<uint8_t> m = mgmtHeader(0xd0, bssid_.data(), bssid_.data());
                m.insert(m.end(), {3, 1, f[26], 37, 0, f[27], f[28], f[29], f[30]});
                enqueue(std::move(m));
            }
            break;
        default:
            break;
        }
    } else if (type == 2 && from_ap && state_.load() == State::Connected) {
        handleData(f, len, packet.RxAtrib.rssi[0], packet.RxAtrib.rssi[1]);
        SignalQualityCalculator::get_instance().add_snr(packet.RxAtrib.snr[0], packet.RxAtrib.snr[1]);
    }
}

void StaLink::handleBeacon(const uint8_t *f, size_t len, uint8_t rssi) {
    constexpr size_t kIeStart = 24 + 12; // timestamp, interval, capability
    if (len < kIeStart) return;
    const uint16_t capability = f[34] | (f[35] << 8);

    std::string ssid;
    bool ssid_match = false;
    int ds_channel = -1;
    bool ht = false;
    bool vht = false;
    bool wmm = false;
    std::vector<uint8_t> rates;
    for (size_t i = kIeStart; i + 2 <= len;) {
        const uint8_t id = f[i];
        const uint8_t l = f[i + 1];
        const uint8_t *v = f + i + 2;
        if (i + 2 + l > len) break;
        if (id == 0) {
            ssid.assign(reinterpret_cast<const char *>(v), l);
            ssid_match = ssid == cfg_.ssid;
        } else if (id == 1 || id == 50) {
            rates.insert(rates.end(), v, v + l);
        } else if (id == 3 && l >= 1) {
            ds_channel = v[0];
        } else if (id == 45) {
            ht = true;
        } else if (id == 191) {
            vht = true;
        } else if (id == 221 && l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 0x02) {
            wmm = true;
        }
        i += 2 + l;
    }

    std::array<uint8_t, 6> bssid;
    std::memcpy(bssid.data(), f + 16, 6);
    if (seen_.size() >= kMaxSeenNetworks && seen_.find(bssid) == seen_.end()) {
        // Forget the stalest entry rather than growing without bound on a busy band.
        auto oldest = std::min_element(
            seen_.begin(), seen_.end(), [](const auto &a, const auto &b) { return a.second.last < b.second.last; });
        seen_.erase(oldest);
    }
    seen_[bssid] =
        SeenNetwork{ssid, ds_channel != -1 ? ds_channel : channel_, (capability & 0x0010) != 0, rssi, Clock::now()};

    if (!ssid_match) return;
    // On 2.4 GHz neighbouring channels leak in; only join on the channel we are tuned to.
    if (ds_channel != -1 && ds_channel != channel_) return;
    if (capability & 0x0010) {
        if (!warned_encrypted_) {
            warned_encrypted_ = true;
            __android_log_print(
                ANDROID_LOG_ERROR, TAG, "\"%s\" is encrypted, only open networks are supported", cfg_.ssid.c_str());
        }
        return;
    }

    std::memcpy(bssid_.data(), f + 16, 6);
    if (home_channel_ != channel_) {
        __android_log_print(ANDROID_LOG_INFO,
                            TAG,
                            "\"%s\" is on ch %d, not on ch %d; looking there first from now on",
                            cfg_.ssid.c_str(),
                            channel_,
                            home_channel_);
        home_channel_ = channel_;
        buildScanList();
    }
    ap_rates_ = std::move(rates);
    ap_ht_ = ht;
    ap_vht_ = vht;
    ap_wmm_ = wmm;
    last_heard_ = Clock::now();
    __android_log_print(ANDROID_LOG_INFO,
                        TAG,
                        "found \"%s\" bssid %02x:%02x:%02x:%02x:%02x:%02x ht=%d vht=%d wmm=%d",
                        cfg_.ssid.c_str(),
                        bssid_[0],
                        bssid_[1],
                        bssid_[2],
                        bssid_[3],
                        bssid_[4],
                        bssid_[5],
                        ht,
                        vht,
                        wmm);
    last_tsf_ = 0;
    for (int i = 7; i >= 0; i--)
        last_tsf_ = (last_tsf_ << 8) | f[24 + i];
    // Straight from the RX callback (only queued here; the worker sends it), which
    // saves up to one tick per handshake step.
    beginAuth();
}

void StaLink::handleAuth(const uint8_t *f, size_t len) {
    if (state_.load() != State::Authenticating || len < 24 + 6) return;
    const uint16_t alg = f[24] | (f[25] << 8);
    const uint16_t seq = f[26] | (f[27] << 8);
    const uint16_t status = f[28] | (f[29] << 8);
    if (alg != 0 || seq != 2) return;
    if (status != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "authentication refused, status %u", status);
        setState(State::Searching);
        return;
    }
    setState(State::Associating);
    attempts_ = 1;
    last_tx_attempt_ = Clock::now();
    sendAssocRequest();
}

void StaLink::handleAssocResp(const uint8_t *f, size_t len) {
    if (state_.load() != State::Associating || len < 24 + 6) return;
    const uint16_t status = f[26] | (f[27] << 8);
    if (status != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "association refused, status %u", status);
        setState(State::Searching);
        return;
    }
    aid_ = (f[28] | (f[29] << 8)) & 0x3fff;
    setState(State::Connected);
    __android_log_print(ANDROID_LOG_INFO, TAG, "associated, AID %u", aid_);
    // Announce our IP right away (gratuitous ARP), then periodically from tick().
    last_garp_ = Clock::now();
    sendArp(1, kZeroMac, cfg_.own_ip.data(), kBroadcast);
}

void StaLink::handleData(const uint8_t *f, size_t len, uint8_t rssi_a, uint8_t rssi_b) {
    const uint8_t fc0 = f[0];
    const uint8_t fc1 = f[1];
    // Only AP -> station frames: FromDS set, ToDS clear.
    if ((fc1 & 0x03) != 0x02) return;
    const uint8_t *addr1 = f + 4;
    const uint8_t *addr3 = f + 16;
    const bool unicast = macEq(addr1, cfg_.own_mac.data());
    if (!unicast && !(addr1[0] & 0x01)) return;

    const uint8_t subtype = fc0 >> 4;
    if (subtype & 0x04) return; // Null / QoS Null: no payload
    const bool qos = subtype & 0x08;
    size_t hlen = 24;
    if (qos) hlen += 2;
    if (qos && (fc1 & 0x80)) hlen += 4; // +HTC
    if (len < hlen + 8) return;

    SignalQualityCalculator::get_instance().add_rssi(rssi_a, rssi_b);

    const uint8_t frag = f[22] & 0x0f;
    if (fc1 & 0x40) {
        // Protected: only open networks are supported.
        sum_protected_++;
        rx_dropped_++;
        return;
    }
    if ((fc1 & 0x04) || frag != 0) {
        // 802.11 fragmentation (the AP's fragmentation threshold): not reassembled.
        sum_mac_fragments_++;
        if (!warned_mac_fragments_) {
            warned_mac_fragments_ = true;
            __android_log_print(ANDROID_LOG_WARN,
                                TAG,
                                "AP sends 802.11 fragments (frag %u, len %zu); these are dropped. "
                                "Check the camera's fragmentation threshold (iw phy ... set frag off).",
                                frag,
                                len);
        }
        rx_dropped_++;
        return;
    }
    if (unicast) {
        const int seq = (f[22] | (f[23] << 8)) >> 4;
        if ((fc1 & 0x08) && seq == last_rx_seq_) return; // retransmission of what we already have
        last_rx_seq_ = seq;
    }
    rx_data_++;
    sum_data_++;

    const uint8_t *p = f + hlen;
    size_t plen = len - hlen;
    const bool amsdu = qos && (f[24] & 0x80);
    if (!amsdu) {
        if (std::memcmp(p, kLlcSnap, 6) != 0) {
            sum_not_ipv4_++;
            rx_dropped_++;
            return;
        }
        handleMsdu((p[6] << 8) | p[7], p + 8, plen - 8, addr3);
        return;
    }
    // A-MSDU: subframes of DA(6) SA(6) length(2) + LLC payload, each padded to 4 bytes.
    while (plen >= 14) {
        const uint8_t *sa = p + 6;
        const size_t sublen = (p[12] << 8) | p[13];
        if (sublen + 14 > plen || sublen < 8) break;
        const uint8_t *sub = p + 14;
        if (std::memcmp(sub, kLlcSnap, 6) == 0) handleMsdu((sub[6] << 8) | sub[7], sub + 8, sublen - 8, sa);
        size_t step = 14 + sublen;
        step = (step + 3) & ~size_t(3);
        if (step >= plen) break;
        p += step;
        plen -= step;
    }
}

void StaLink::handleMsdu(uint16_t ethertype, const uint8_t *payload, size_t len, const uint8_t *src_mac) {
    if (ethertype == kEthArp) {
        handleArp(payload, len);
    } else if (ethertype == kEthIpv4) {
        handleIpv4(payload, len, src_mac);
    } else {
        sum_not_ipv4_++; // IPv6 and the like: nothing here uses them
    }
}

// Called with mu_ held. RFC 791 reassembly, kept small: at most 8 datagrams in
// flight, each dropped 1 s after its first fragment. Which 8-byte units have
// arrived is tracked, so duplicates and overlaps cannot complete a datagram that
// still has holes.
std::vector<uint8_t> StaLink::reassemble(const uint8_t *ip, size_t ihl, size_t len) {
    const auto now = Clock::now();
    frags_.erase(
        std::remove_if(frags_.begin(), frags_.end(), [now](const IpFragments &f) { return now - f.start > 1s; }),
        frags_.end());

    uint32_t src;
    std::memcpy(&src, ip + 12, 4);
    const uint16_t id = (ip[4] << 8) | ip[5];
    const uint16_t fo = (ip[6] << 8) | ip[7];
    const bool more = fo & 0x2000;
    const size_t off = static_cast<size_t>(fo & 0x1fff) * 8;
    const size_t plen = len - ihl;
    if (off + plen + ihl > 65535) return {};
    if (more && (plen % 8) != 0) return {}; // only the last fragment may end off a unit boundary

    auto it = std::find_if(
        frags_.begin(), frags_.end(), [src, id](const IpFragments &f) { return f.src == src && f.id == id; });
    if (it == frags_.end()) {
        if (frags_.size() >= 8) frags_.erase(frags_.begin());
        frags_.push_back(IpFragments{src, id, {}, {}, {}, 0, now});
        it = frags_.end() - 1;
    }
    if (it->payload.size() < off + plen) it->payload.resize(off + plen);
    std::memcpy(it->payload.data() + off, ip + ihl, plen);
    const size_t first_unit = off / 8, end_unit = (off + plen + 7) / 8;
    if (it->got.size() < end_unit) it->got.resize(end_unit, false);
    for (size_t u = first_unit; u < end_unit; u++)
        it->got[u] = true;
    if (off == 0) it->header.assign(ip, ip + ihl);
    if (!more) it->total = off + plen;
    if (it->total == 0 || it->header.empty()) return {};
    const size_t units = (it->total + 7) / 8;
    if (it->got.size() < units) return {};
    for (size_t u = 0; u < units; u++) {
        if (!it->got[u]) return {};
    }

    std::vector<uint8_t> whole(it->header);
    whole.insert(whole.end(), it->payload.begin(), it->payload.begin() + it->total);
    const size_t total_len = whole.size();
    whole[2] = static_cast<uint8_t>(total_len >> 8);
    whole[3] = static_cast<uint8_t>(total_len & 0xff);
    whole[6] = whole[7] = 0; // no longer a fragment
    frags_.erase(it);
    return whole;
}

void StaLink::handleArp(const uint8_t *a, size_t len) {
    if (len < 28) return;
    // Ethernet / IPv4 only.
    if (a[0] != 0 || a[1] != 1 || a[2] != 0x08 || a[3] != 0x00 || a[4] != 6 || a[5] != 4) return;
    const uint16_t oper = (a[6] << 8) | a[7];
    const uint8_t *sha = a + 8;
    const uint8_t *spa = a + 14;
    const uint8_t *tpa = a + 24;
    learnMac(spa, sha);
    if (oper != 1) return;
    if (std::memcmp(tpa, cfg_.own_ip.data(), 4) == 0) {
        sendArp(2, sha, spa, sha);
        return;
    }
    // Someone looks for an address that is not ours. If that is the camera trying to
    // reach its stream target, the phone IP in the AP settings is wrong.
    uint32_t target;
    std::memcpy(&target, tpa, 4);
    if (warned_arp_targets_.size() < 16 && warned_arp_targets_.insert(target).second) {
        __android_log_print(ANDROID_LOG_WARN,
                            TAG,
                            "%d.%d.%d.%d asks for %d.%d.%d.%d (our IP is %d.%d.%d.%d). "
                            "If that is the camera's stream target, set it as the phone IP.",
                            spa[0],
                            spa[1],
                            spa[2],
                            spa[3],
                            tpa[0],
                            tpa[1],
                            tpa[2],
                            tpa[3],
                            cfg_.own_ip[0],
                            cfg_.own_ip[1],
                            cfg_.own_ip[2],
                            cfg_.own_ip[3]);
    }
}

void StaLink::handleIpv4(const uint8_t *ip, size_t len, const uint8_t *src_mac) {
    if (len < 20 || (ip[0] >> 4) != 4) return;
    const size_t ihl = (ip[0] & 0x0f) * 4;
    const size_t total = (ip[2] << 8) | ip[3];
    if (ihl < 20 || total < ihl || total > len) return;
    len = total; // drop link-layer padding

    const uint8_t *dst = ip + 16;
    const bool for_us = std::memcmp(dst, cfg_.own_ip.data(), 4) == 0;
    const bool bcast = std::memcmp(dst, kBroadcast, 4) == 0;
    if (!for_us && !bcast) return;

    const uint16_t frag = (ip[6] << 8) | ip[7];
    if (frag & 0x3fff) {
        // More-fragments flag or a non-zero offset: datagrams larger than the MTU,
        // e.g. when the sender's payload size exceeds ~1470 bytes.
        sum_fragments_++;
        std::vector<uint8_t> whole = reassemble(ip, ihl, len);
        if (whole.empty()) return;
        sum_reassembled_++;
        handleIpv4(whole.data(), whole.size(), src_mac);
        return;
    }

    const uint8_t proto = ip[9];
    const uint8_t *l4 = ip + ihl;
    const size_t l4len = len - ihl;
    if (for_us) learnMac(ip + 12, src_mac);

    if (proto == 6 && for_us) {
        handleTcp(ip, ihl, len);
        return;
    }

    if (proto == 17 && l4len >= 8) { // UDP
        const uint16_t dport = (l4[2] << 8) | l4[3];
        const size_t ulen = (l4[4] << 8) | l4[5];
        if (ulen < 8 || ulen > l4len) return;
        if (for_us) {
            udp_by_port_[dport]++;
            if (seen_udp_ports_.size() < 32 && seen_udp_ports_.insert(dport).second) {
                __android_log_print(ANDROID_LOG_INFO,
                                    TAG,
                                    "first UDP datagram for port %d: from %d.%d.%d.%d:%d, %zu bytes",
                                    dport,
                                    ip[12],
                                    ip[13],
                                    ip[14],
                                    ip[15],
                                    (l4[0] << 8) | l4[1],
                                    ulen - 8);
            }
        }
        bool forwarded = false;
        for (const auto &fwd : cfg_.forward_ports) {
            if (fwd.first != dport) continue;
            forwarded = true;
            if (!have_camera_ip_ && for_us) {
                have_camera_ip_ = true;
                std::memcpy(camera_ip_.data(), ip + 12, 4);
                __android_log_print(ANDROID_LOG_INFO,
                                    TAG,
                                    "receiving UDP from %d.%d.%d.%d:%d on port %d",
                                    ip[12],
                                    ip[13],
                                    ip[14],
                                    ip[15],
                                    (l4[0] << 8) | l4[1],
                                    dport);
            }
            if (udp_sock_ >= 0) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(fwd.second);
                to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                sendto(udp_sock_, l4 + 8, ulen - 8, MSG_DONTWAIT, (sockaddr *)&to, sizeof(to));
            }
            rx_udp_++;
            break;
        }
        if (!forwarded && for_us && warned_ports_.size() < 16 && warned_ports_.insert(dport).second) {
            __android_log_print(ANDROID_LOG_WARN,
                                TAG,
                                "UDP from %d.%d.%d.%d to port %d is not forwarded (video port setting?)",
                                ip[12],
                                ip[13],
                                ip[14],
                                ip[15],
                                dport);
        }
    } else if (proto == 1 && for_us && l4len >= 8 && len <= kMaxEchoDatagram && l4[0] == 8) { // ICMP echo request
        std::vector<uint8_t> r(ip, ip + len);
        std::memcpy(r.data() + 16, ip + 12, 4); // dst = original src
        std::memcpy(r.data() + 12, cfg_.own_ip.data(), 4);
        r[8] = 64; // TTL
        r[10] = r[11] = 0;
        uint16_t ic = csum16(r.data(), ihl);
        r[10] = ic >> 8;
        r[11] = ic & 0xff;
        r[ihl] = 0; // echo reply
        r[ihl + 2] = r[ihl + 3] = 0;
        uint16_t cc = csum16(r.data() + ihl, l4len);
        r[ihl + 2] = cc >> 8;
        r[ihl + 3] = cc & 0xff;
        sendData(src_mac, kEthIpv4, r.data(), r.size());
    }
}

// ---------------------------------------------------------------------------
// Minimal TCP client, for the camera's control port. One connection at a time,
// in-order receive only, fixed retransmission timer. Called with mu_ held unless
// noted.
// ---------------------------------------------------------------------------

void StaLink::learnMac(const uint8_t *ip, const uint8_t *mac) {
    if (ipKey(ip) == 0 || (mac[0] & 0x01)) return;
    if (arp_cache_.size() > 64) arp_cache_.clear();
    auto &slot = arp_cache_[ipKey(ip)];
    std::memcpy(slot.data(), mac, 6);
    if (tcp_.st == TcpSession::St::Resolving && std::memcmp(tcp_.ip.data(), ip, 4) == 0) {
        tcp_.mac = slot;
        tcp_.st = TcpSession::St::SynSent;
        tcp_.retries = 0;
        tcp_.last_tx = Clock::now();
        tcpSend(kTcpSyn, tcp_.iss, nullptr, 0);
    }
}

// Not called with mu_ held: blocks until the exchange is over.
bool StaLink::tcpRequest(const std::array<uint8_t, 4> &ip,
                         uint16_t port,
                         const std::string &request,
                         std::chrono::milliseconds timeout,
                         bool half_close,
                         std::string &reply) {
    using St = TcpSession::St;
    std::unique_lock<std::mutex> lock(mu_);
    // One session slot: a caller that finds it busy (e.g. the key-frame request after a
    // recovery) waits its turn instead of being told the camera did not answer.
    // `timeout` covers the wait and the exchange together, so the caller never waits longer.
    const auto deadline = Clock::now() + timeout;
    const auto usable = [this] { return running_ && state_.load() == State::Connected; };
    if (!tcp_cv_.wait_until(lock, deadline, [&] { return !usable() || tcp_.st == St::Idle; })) return false;
    if (!usable()) return false;

    std::random_device rd;
    TcpSession s;
    s.ip = ip;
    s.rport = port;
    s.lport = static_cast<uint16_t>(49152 + rd() % 16000);
    s.iss = rd();
    s.snd_nxt = s.iss + 1;
    s.half_close = half_close;
    s.data.assign(request.begin(), request.end());
    s.deadline = deadline;
    s.last_tx = Clock::now();
    tcp_ = std::move(s);

    __android_log_print(
        ANDROID_LOG_INFO, TAG, "TCP %d.%d.%d.%d:%d <- \"%s\"", ip[0], ip[1], ip[2], ip[3], port, request.c_str());
    auto known = arp_cache_.find(ipKey(ip.data()));
    if (known != arp_cache_.end()) {
        tcp_.mac = known->second;
        tcp_.st = St::SynSent;
        tcpSend(kTcpSyn, tcp_.iss, nullptr, 0);
    } else {
        tcp_.st = St::Resolving;
        sendArp(1, kZeroMac, ip.data(), kBroadcast);
    }

    // tcpTick() enforces the deadline; the extra margin only guards against a worker that died.
    const auto done = [this] { return tcp_.st == St::Done || tcp_.st == St::Failed; };
    if (!tcp_cv_.wait_until(lock, tcp_.deadline + 2s, done)) tcpFinish(false);

    reply = tcp_.rx;
    __android_log_print(ANDROID_LOG_INFO,
                        TAG,
                        "TCP %d.%d.%d.%d:%d -> %s \"%s\"",
                        ip[0],
                        ip[1],
                        ip[2],
                        ip[3],
                        port,
                        tcp_.st == St::Done ? "done" : "failed",
                        reply.c_str());
    const bool ok = tcp_.st == St::Done && !reply.empty();
    tcp_.st = St::Idle;
    tcp_cv_.notify_all(); // the slot is free for the next caller
    return ok;
}

void StaLink::tcpFinish(bool ok) {
    using St = TcpSession::St;
    if (tcp_.st == St::Idle || tcp_.st == St::Done || tcp_.st == St::Failed) return;
    if (tcp_.st == St::Established && !tcp_.half_close) {
        // Close our side (a half-closing request already sent its FIN with the data);
        // whatever the peer still sends is ignored from here on.
        tcpSend(kTcpFin | kTcpAck, tcp_.snd_nxt + static_cast<uint32_t>(tcp_.data.size()), nullptr, 0);
    }
    tcp_.st = ok ? St::Done : St::Failed;
    tcp_cv_.notify_all();
}

void StaLink::tcpTick(Clock::time_point now) {
    using St = TcpSession::St;
    if (tcp_.st != St::Resolving && tcp_.st != St::SynSent && tcp_.st != St::Established) return;
    if (now >= tcp_.deadline) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "TCP request timed out");
        // A cut-off HTTP reply is a failure, so the caller retries instead of parsing half a body.
        tcpFinish(!tcp_.rx.empty() && !httpReplyIncomplete(tcp_.rx));
        return;
    }
    if (tcp_.st == St::Established && !tcp_.rx.empty() && now - tcp_.last_rx >= kTcpReplyIdle &&
        !httpReplyIncomplete(tcp_.rx)) {
        tcpFinish(true);
        return;
    }
    if (now - tcp_.last_tx < kTcpRetransmit) return;
    tcp_.last_tx = now;
    tcp_.retries++;
    switch (tcp_.st) {
    case St::Resolving:
        sendArp(1, kZeroMac, tcp_.ip.data(), kBroadcast);
        break;
    case St::SynSent:
        tcpSend(kTcpSyn, tcp_.iss, nullptr, 0);
        break;
    case St::Established:
        if (!tcp_.data_acked) {
            tcpSend(
                kTcpAck | kTcpPsh | (tcp_.half_close ? kTcpFin : 0), tcp_.snd_nxt, tcp_.data.data(), tcp_.data.size());
        }
        break;
    default:
        break;
    }
}

void StaLink::handleTcp(const uint8_t *ip, size_t ihl, size_t len) {
    using St = TcpSession::St;
    if (tcp_.st != St::SynSent && tcp_.st != St::Established) return;
    if (len < ihl + 20 || std::memcmp(ip + 12, tcp_.ip.data(), 4) != 0) return;
    const uint8_t *t = ip + ihl;
    const uint16_t sport = (t[0] << 8) | t[1];
    const uint16_t dport = (t[2] << 8) | t[3];
    if (sport != tcp_.rport || dport != tcp_.lport) return;
    const uint32_t seq = rd32(t + 4);
    const uint32_t ack = rd32(t + 8);
    const size_t doff = (t[12] >> 4) * 4;
    const uint8_t flags = t[13];
    if (doff < 20 || ihl + doff > len) return;
    const uint8_t *payload = t + doff;
    const size_t plen = len - ihl - doff;
    const auto now = Clock::now();
    const uint32_t data_end = tcp_.snd_nxt + static_cast<uint32_t>(tcp_.data.size());
    // Our next sequence number once the request is out: a half-closing request's FIN
    // takes one more.
    const uint32_t our_end = data_end + (tcp_.half_close ? 1 : 0);

    if (flags & kTcpRst) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "TCP connection reset by the peer");
        tcp_.st = tcp_.rx.empty() || httpReplyIncomplete(tcp_.rx) ? St::Failed : St::Done;
        tcp_cv_.notify_all();
        return;
    }

    if (tcp_.st == St::SynSent) {
        if ((flags & (kTcpSyn | kTcpAck)) != (kTcpSyn | kTcpAck) || ack != tcp_.iss + 1) return;
        tcp_.rcv_nxt = seq + 1;
        tcp_.st = St::Established;
        tcp_.retries = 0;
        tcp_.last_tx = now;
        // The ACK of the handshake rides on the request. A half-closing request ends
        // our side right away, like `echo cmd | nc` does: some line servers only act
        // on end of input.
        tcpSend(kTcpAck | kTcpPsh | (tcp_.half_close ? kTcpFin : 0), tcp_.snd_nxt, tcp_.data.data(), tcp_.data.size());
        return;
    }

    if ((flags & kTcpAck) && static_cast<int32_t>(ack - our_end) >= 0) tcp_.data_acked = true;
    if (plen > 0 && seq == tcp_.rcv_nxt) {
        if (tcp_.rx.size() + plen > kTcpMaxReply) {
            // Not a camera reply; give up now instead of re-ACKing until the deadline.
            __android_log_print(ANDROID_LOG_WARN, TAG, "TCP reply over %zu bytes, dropping it", kTcpMaxReply);
            tcpFinish(false);
            return;
        }
        tcp_.rx.append(reinterpret_cast<const char *>(payload), plen);
        tcp_.rcv_nxt += static_cast<uint32_t>(plen);
        tcp_.last_rx = now;
    }
    if ((flags & kTcpFin) && seq + plen == tcp_.rcv_nxt) {
        // Peer is done: acknowledge its FIN, together with ours unless that went
        // out with the request.
        tcp_.rcv_nxt += 1;
        if (tcp_.half_close) {
            tcpSend(kTcpAck, our_end, nullptr, 0);
        } else {
            tcpSend(kTcpFin | kTcpAck, data_end, nullptr, 0);
        }
        // Closed before the Content-Length was reached: a cut-off reply, not an answer.
        tcp_.st = httpReplyIncomplete(tcp_.rx) ? St::Failed : St::Done;
        tcp_cv_.notify_all();
        return;
    }
    // Acknowledge new data, and re-acknowledge duplicates or out-of-order segments
    // so the peer retransmits from rcv_nxt.
    if (plen > 0) tcpSend(kTcpAck, our_end, nullptr, 0);
}

void StaLink::tcpSend(uint8_t flags, uint32_t seq, const uint8_t *data, size_t len) {
    const bool syn = flags & kTcpSyn;
    const size_t hlen = syn ? 24 : 20;
    std::vector<uint8_t> t(hlen + len, 0);
    t[0] = tcp_.lport >> 8;
    t[1] = tcp_.lport & 0xff;
    t[2] = tcp_.rport >> 8;
    t[3] = tcp_.rport & 0xff;
    wr32(&t[4], seq);
    wr32(&t[8], (flags & kTcpAck) ? tcp_.rcv_nxt : 0);
    t[12] = static_cast<uint8_t>((hlen / 4) << 4);
    t[13] = flags;
    t[14] = 0x20; // window 8192
    t[15] = 0x00;
    if (syn) {
        // MSS 1460
        t[20] = 2;
        t[21] = 4;
        t[22] = 0x05;
        t[23] = 0xb4;
    }
    if (len) std::memcpy(&t[hlen], data, len);

    uint8_t pseudo[12];
    std::memcpy(pseudo, cfg_.own_ip.data(), 4);
    std::memcpy(pseudo + 4, tcp_.ip.data(), 4);
    pseudo[8] = 0;
    pseudo[9] = 6;
    pseudo[10] = static_cast<uint8_t>(t.size() >> 8);
    pseudo[11] = static_cast<uint8_t>(t.size() & 0xff);
    const uint16_t c = csumFold(csumAdd(csumAdd(0, pseudo, sizeof(pseudo)), t.data(), t.size()));
    t[16] = c >> 8;
    t[17] = c & 0xff;
    sendIpv4(tcp_.mac.data(), tcp_.ip.data(), 6, t);
}

void StaLink::sendIpv4(const uint8_t *dst_mac, const uint8_t *dst_ip, uint8_t proto, const std::vector<uint8_t> &l4) {
    std::vector<uint8_t> p(20 + l4.size(), 0);
    const size_t total = p.size();
    ip_id_++;
    p[0] = 0x45;
    p[2] = static_cast<uint8_t>(total >> 8);
    p[3] = static_cast<uint8_t>(total & 0xff);
    p[4] = ip_id_ >> 8;
    p[5] = ip_id_ & 0xff;
    p[6] = 0x40; // don't fragment
    p[8] = 64;   // TTL
    p[9] = proto;
    std::memcpy(&p[12], cfg_.own_ip.data(), 4);
    std::memcpy(&p[16], dst_ip, 4);
    const uint16_t c = csum16(p.data(), 20);
    p[10] = c >> 8;
    p[11] = c & 0xff;
    std::memcpy(&p[20], l4.data(), l4.size());
    sendData(dst_mac, kEthIpv4, p.data(), p.size());
}

// ---------------------------------------------------------------------------
// TX frame builders. All are called with mu_ held.
// ---------------------------------------------------------------------------

uint16_t StaLink::nextSeq() {
    seq_ = (seq_ + 1) & 0x0fff;
    return seq_ << 4;
}

std::vector<uint8_t> StaLink::mgmtHeader(uint8_t fc0, const uint8_t *addr1, const uint8_t *addr3) {
    const uint8_t *own = cfg_.own_mac.data();
    const uint16_t seq = nextSeq();
    return {fc0,
            0x00,
            0x00,
            0x00,
            addr1[0],
            addr1[1],
            addr1[2],
            addr1[3],
            addr1[4],
            addr1[5],
            own[0],
            own[1],
            own[2],
            own[3],
            own[4],
            own[5],
            addr3[0],
            addr3[1],
            addr3[2],
            addr3[3],
            addr3[4],
            addr3[5],
            (uint8_t)(seq & 0xff),
            (uint8_t)(seq >> 8)};
}

// Supported Rates (max 8) + Extended Supported Rates. Mirrors the AP's own set
// once we know it, otherwise a band-correct default (no CCK on 5 GHz).
void StaLink::appendRates(std::vector<uint8_t> &m) const {
    std::vector<uint8_t> rates = ap_rates_;
    if (rates.empty()) {
        if (channel_ <= 14) {
            rates = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
        } else {
            rates = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
        }
    }
    const size_t first = std::min<size_t>(rates.size(), 8);
    m.push_back(1);
    m.push_back((uint8_t)first);
    m.insert(m.end(), rates.begin(), rates.begin() + first);
    if (rates.size() > first) {
        m.push_back(50);
        m.push_back((uint8_t)(rates.size() - first));
        m.insert(m.end(), rates.begin() + first, rates.end());
    }
}

void StaLink::sendProbeRequest() {
    std::vector<uint8_t> m = mgmtHeader(0x40, kBroadcast, kBroadcast);
    m.push_back(0);
    m.push_back((uint8_t)cfg_.ssid.size());
    m.insert(m.end(), cfg_.ssid.begin(), cfg_.ssid.end());
    appendRates(m);
    enqueue(std::move(m));
}

void StaLink::sendAuth() {
    std::vector<uint8_t> m = mgmtHeader(0xb0, bssid_.data(), bssid_.data());
    m.insert(m.end(), {0x00, 0x00, 0x01, 0x00, 0x00, 0x00}); // open system, seq 1, status 0
    enqueue(std::move(m));
}

void StaLink::sendAssocRequest() {
    std::vector<uint8_t> m = mgmtHeader(0x00, bssid_.data(), bssid_.data());
    // Capability: ESS, plus short preamble / short slot on 2.4 GHz.
    const uint16_t cap = 0x0001 | (channel_ <= 14 ? 0x0420 : 0x0000);
    m.insert(m.end(), {(uint8_t)(cap & 0xff), (uint8_t)(cap >> 8), 0x0a, 0x00}); // listen interval 10
    m.push_back(0);
    m.push_back((uint8_t)cfg_.ssid.size());
    m.insert(m.end(), cfg_.ssid.begin(), cfg_.ssid.end());
    appendRates(m);
    // Capabilities are advertised up to the configured width, not the AP's current
    // one, so a later switch to a wider channel is used without re-associating.
    const bool wide5g = channel_ > 14;
    const bool can40 = wide5g && cfg_.max_width_mhz >= 40;
    const bool can80 = wide5g && cfg_.max_width_mhz >= 80 && ap_vht_;
    if (ap_ht_) {
        // HT Capabilities: SM power save disabled, short GI 20, RX MCS 0-7, plus
        // 20/40 MHz and short GI 40 when allowed. Lets the AP use HT rates towards us.
        uint16_t ht_cap = 0x002c;
        if (can40) ht_cap |= 0x0002 | 0x0040;
        m.insert(m.end(), {45, 26, static_cast<uint8_t>(ht_cap & 0xff), static_cast<uint8_t>(ht_cap >> 8), 0x03});
        uint8_t mcs[16] = {0xff};
        m.insert(m.end(), mcs, mcs + 16);
        m.insert(m.end(), {0, 0, 0, 0, 0, 0, 0});
    }
    if (can80) {
        // VHT Capabilities: max MPDU 3895, 80 MHz, short GI 80; RX/TX 1SS MCS 0-7
        // (MCS map 0xfffc: 1SS = 0-7, other streams not supported).
        m.insert(m.end(), {191, 12, 0x20, 0x00, 0x00, 0x00, 0xfc, 0xff, 0x00, 0x00, 0xfc, 0xff, 0x00, 0x00});
    }
    if (ap_wmm_) {
        // WMM Information Element. hostapd only enables HT for WMM stations.
        m.insert(m.end(), {221, 7, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00});
    }
    enqueue(std::move(m));
}

void StaLink::sendArp(uint16_t oper, const uint8_t *target_mac, const uint8_t *target_ip, const uint8_t *dst_mac) {
    const uint8_t *own = cfg_.own_mac.data();
    const uint8_t *ip = cfg_.own_ip.data();
    const uint8_t a[28] = {0x00,
                           0x01,
                           0x08,
                           0x00,
                           6,
                           4,
                           (uint8_t)(oper >> 8),
                           (uint8_t)(oper & 0xff),
                           own[0],
                           own[1],
                           own[2],
                           own[3],
                           own[4],
                           own[5],
                           ip[0],
                           ip[1],
                           ip[2],
                           ip[3],
                           target_mac[0],
                           target_mac[1],
                           target_mac[2],
                           target_mac[3],
                           target_mac[4],
                           target_mac[5],
                           target_ip[0],
                           target_ip[1],
                           target_ip[2],
                           target_ip[3]};
    sendData(dst_mac, kEthArp, a, sizeof(a));
}

void StaLink::sendData(const uint8_t *dst_mac, uint16_t ethertype, const uint8_t *payload, size_t len) {
    // ToDS data frame: addr1 = BSSID, addr2 = us, addr3 = final destination.
    std::vector<uint8_t> m = mgmtHeader(0x08, bssid_.data(), dst_mac);
    m[1] = 0x01;
    m.insert(m.end(), kLlcSnap, kLlcSnap + 6);
    m.push_back((uint8_t)(ethertype >> 8));
    m.push_back((uint8_t)(ethertype & 0xff));
    m.insert(m.end(), payload, payload + len);
    enqueue(std::move(m));
}

void StaLink::enqueue(std::vector<uint8_t> mpdu) {
    std::vector<uint8_t> frame;
    frame.reserve(radiotap_.size() + mpdu.size());
    frame.insert(frame.end(), radiotap_.begin(), radiotap_.end());
    frame.insert(frame.end(), mpdu.begin(), mpdu.end());
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        if (tx_queue_.size() < 128) tx_queue_.push_back(std::move(frame));
    }
    q_cv_.notify_all();
}
