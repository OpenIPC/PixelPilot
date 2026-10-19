# PixelPilot
> [!IMPORTANT]
> Warning, performance will heavily depend on your device's processing power.
>
> Use this app at your own risk.

## Introduction
```
PixelPilot – the Android FPV app that leaves “loading…” screens in the dust.
Plug in, fly live with sub‑atomic latency, flex real‑time signal stats, and marvel at the open‑source unicorn
where (brace yourself) most things actually work.
```

- [FPVue_android](https://github.com/gehee/FPVue_android): basic and unique work to combine all components into a single application by [Gee He](https://github.com/gehee).
- [devourer](https://github.com/openipc/devourer): userspace Realtek 11ac driver (rtl8812au + rtl8812eu) initially created by [buldo](https://github.com/buldo) and converted to C by [josephnef](https://github.com/josephnef).
- [LiveVideo10ms](https://github.com/Consti10/LiveVideo10ms): excellent video decoder from [Consti10](https://github.com/Consti10) converted into a module.
- [wfb-ng](https://github.com/svpcom/wfb-ng): library allowing the broadcast of the video feed over the air.

The wfb-ng [gs.key](https://github.com/OpenIPC/PixelPilot/raw/master/app/src/main/assets/gs.key) is embedded in the app.
The settings menu allows selecting a different key from your phone.

Supported rtl8812au and rtl8812eu wifi adapters are listed [here](https://github.com/OpenIPC/PixelPilot/blob/master/app/src/main/res/xml/usb_device_filter.xml).
Feel free to send pull requests to add new supported wifi adapters hardware IDs.

Now support saving a dvr of the video feed to `Files/Internal Storage/Movies/`

## Compatibility
- arm64-v8a, armeabi-v7a android devices (including Meta Quest 2/3, non vr mode)

## Build
```
git clone https://github.com/OpenIPC/PixelPilot.git
cd PixelPilot
git submodule init
git submodule update
```

The project can then be opened in android studio and built from there.

## Installation
- Download and install PixelPilot.apk from https://github.com/OpenIPC/PixelPilot/releases
- Audio feature: Now PixelPilot app had ability to play opus stream from majestic on camera. In order to enable this feature, pls enable on camera side:
+ Audio settings in (/etc/majestic.yaml):
```
audio:
  enabled: true
  volume: 30
  srate: 8000
  codec: opus
  outputEnabled: false
  outputVolume: 30
```

## Link modes: WFB-NG and APFPV

Settings → **Link mode** switches how the wifi adapter is used:

- **WFB-NG** (default): wfb-ng broadcast in monitor mode, as before.
- **APFPV**: the adapter joins the camera's own **open** (unencrypted) Wi-Fi access point as a
  regular station, driven entirely by the devourer userspace driver. The phone uses a static IP
  and the camera pushes its RTP stream there. Made for the OpenIPC apfpv firmware.

**APFPV settings...** holds the SSID, the phone's static IP, the video UDP port and the adapter's
MAC address (generated once, locally administered), the camera's IP and the maximum channel width.
Defaults match the OpenIPC apfpv firmware (camera `192.168.0.1`, ground station `192.168.0.10`,
video to port 5600).

Finding the camera without a scan: the app starts on the channel the camera was last seen on
(remembered automatically), then checks the channels the apfpv camera passes through while booting
(157, then 165, before `air_man_ap` moves to `wlanchan`). Channel switch announcements from the
camera's access point (hostapd `chan_switch`) are followed without reconnecting. A sweep of all
5 GHz and 2.4 GHz channels (DFS channels passively) is optional in **APFPV settings**. Received UDP
datagrams for the video port are handed to the player, port 14550 to the MAVLink OSD.

In APFPV mode the **Channel** menu changes the camera's channel: while connected it sends
`set_ap_channel <ch>` to the camera's `air_man_ap` (TCP 12355), the camera announces the switch
and the app follows, so `air_man_ap`'s ping check afterwards succeeds. The camera's reply is shown
as a toast. Without a connection the menu only changes where the next search starts.

Channel width: the app follows the camera's channel width up to **Max. channel width** in APFPV
settings (20/40/80 MHz, default 80). It reads the width from the camera's beacons and from its
channel switch announcements, so a switch to a 40/80 MHz channel is followed at the new width
right away (`air_man_ap` pings the ground station right after a switch and reverts when that
fails). 80 MHz requires the primary to be the lowest 20 MHz of the block, which every 80 MHz
channel `air_man_ap` offers is. If a wide channel does not work with an adapter, set the maximum
to 20 MHz: the app then only advertises 20 MHz and the camera sends to it at 20 MHz.

Camera control: in APFPV mode **Drone → Settings** opens the camera's controls instead of the web
page: channel and width, video mode, region (`air_man_ap`, TCP 12355, no login) and image options
(flip, overshoot fix, HQ i-frames, focus mode), aalink link parameters (max MCS per width, link
resilience, max throughput, LQ consideration, temperature threshold, VTX power, OSD size),
flight-controller port, CPU overclock, SSID, hostapd start channel and reboot (the camera's
aalinkFPV web API, with the Drone login credentials). They go over the USB adapter while it is
joined to the camera (a WebView could not reach it there, it uses the phone's network), otherwise
over the phone's own Wi-Fi, for when the phone itself is connected to the camera. In WFB-NG mode
Settings opens the web page at **Drone → Address** (default `10.5.0.10`).

Dropouts: losing the signal does not end the association. The app stays on the channel and takes
video again the moment the camera is heard, with nothing to rebuild. It only rejoins when the camera
says it no longer knows the app (deauth, also sent in answer to the keepalive that goes out as soon
as the camera is heard again) or the beacon timestamp shows the camera restarted; the rejoin goes
straight to the known BSSID on the current channel, without searching. After 3 s of silence the
app also glances at the camera's boot channels (150 ms each, between 1 s on the home channel).
Whenever video can flow again, the app asks majestic for a key frame (`GET /request/idr`, with the
Drone login credentials), so the picture does not wait for the next scheduled one.

Until video arrives, the connection state is shown in the middle of the screen, including the
networks the adapter hears on the current channel. Logcat tag `pixelpilot-sta` has the details.

The stock apfpv firmware runs a WPA2 access point with DHCP. `tools/apfpv-open-static-cam.sh`
prepares the camera for this mode: it patches `/usr/bin/adapter` (which writes `hostapd.conf`)
for an open network without DHCP and with `disassoc_low_ack=0`, so a fade does not end the
association, optionally with a MAC filter for the adapter. It keeps a backup (`--revert`):
```
ssh root@192.168.0.1 sh -s < tools/apfpv-open-static-cam.sh
```

On the camera, send the stream to the phone's IP, e.g. in `/etc/majestic.yaml`:
```
outgoing:
  enabled: true
  server: udp://192.168.0.10:5600
```

Limitations: open networks only (no WEP/WPA), no DHCP (static IP), no 802.11 fragment reassembly,
UDP only, plus one short TCP request at a time for the camera's control port. The adapter answers
ARP and ping for its IP and sends a keepalive every 5 s. On RTL8812AU adapters devourer sends every
frame without hardware retries, so the few frames the phone sends (association, ARP) rely on
software retries; the video direction is acknowledged in hardware on all chips.

Security: the camera's access point is open, without encryption and without protected
management frames (PMF), so everything on it can be read and forged by anyone in radio range.
Video and MAVLink go out unencrypted. A spoofed deauth makes the app rejoin, and a spoofed beacon
with the camera's SSID can draw it to another channel or access point. Install with a MAC filter
for the adapter (its address is in **APFPV settings**), so other stations cannot join the camera:
```
ssh root@192.168.0.1 sh -s -- --allow-mac aa:bb:cc:dd:ee:ff < tools/apfpv-open-static-cam.sh
```
The filter only keeps honest stations out; an attacker can copy the adapter's address from the air.
The Drone login is sent as HTTP Basic auth, i.e. in cleartext, with every camera control request
and with the key-frame request after each (re)association and each end of a dropout. On the
apfpv firmware that is usually the camera's root login: use APFPV mode only where nobody else is
listening, or give the camera a password that is not used anywhere else.

## Quick Reference: Auto-FEC Levels 0 – 5

| Level | Denominator | Added Redundancy |
|-------|-------------|------------------|
| 0 | **1.00000** | 0 % |
| 1 | **1.11111** | ≈ 11 % |
| 2 | **1.25000** | 25 % |
| 3 | **1.42000** | 42 % |
| 4 | **1.66667** | 67 % |
| 5 | **2.00000** | 100 % |

> **How the denominator is used:**  
> The selected denominator multiplies either **`FEC_k`** (source-packet count) *or* **`FEC_n`** (total packets after redundancy), increasing the actual amount of forward-error-correction data.


### Threshold Fields (packets / second)

| Field | Purpose |
|-------|---------|
| `LostThreshold` | If `lost_pkts ≥ LostThreshold`, jump straight to **FEC-5** |
| `RecThr1 … RecThr4` | Number of *recovered* packets (`rec_pkts`) that triggers **FEC-1 … FEC-4** |

**Ordering constraint — must hold:**  
`RecThr1 < RecThr2 < RecThr3 < RecThr4 < LostThreshold`

---

### Decision Logic (executed once per second)

```text
if lost_pkts >= LostThreshold:
    level = 5
elif rec_pkts >= RecThr4:
    level = 4
elif rec_pkts >= RecThr3:
    level = 3
elif rec_pkts >= RecThr2:
    level = 2
elif rec_pkts >= RecThr1:
    level = 1
else:
    level = 0

apply_fec(level)  # multiply the level’s denominator by FEC_k or FEC_n
```

Set thresholds thoughtfully:
Lower values → more aggressive protection (higher bandwidth / latency).
Higher values → leaner bandwidth, less resilience.

---

## Object Detection (Optional)
PixelPilot features an optional, real-time object detection overlay using Google's MediaPipe Object Detection library.

### Features
* **Live Overlay**: Draws bounding boxes, class names, and confidence scores directly over the standard video stream.
* **Hardware Acceleration**: Toggle between CPU and GPU delegates to optimize performance for your device.
* **Model Choices**: Supports switching between `EfficientDet-Lite0` (lightweight/faster) and `EfficientDet-Lite2` (larger/more accurate) models.
* **Automated Setup**: The necessary TFLite model files are downloaded automatically from Google's CDN during the Gradle build phase.

### How to Use
1. In the application settings menu, click on the **Object Detection** submenu.
2. Select **Enable** to toggle the detection overlay on/off.
3. Choose your preferred hardware **Delegate** (CPU/GPU) and **Model** (Lite0/Lite2) based on your device's processing capabilities.

---

## List of potential improvements:
 * adaptive link [x]
 * 40 MHz bandwidth [?] - works but buggy
 * support stream over ipv6
 * Save audio stream with the video for recordings
 * Possibility to forward undecoded wfb stream over the network

## Known issues:
 * Audio stream is not working

## Tested devices based on real user reviews

* Samsung Galaxy A54 (Exynos 1380 processor)
* Google Pixel 7 Pro
* Poco x6 Pro
* Meta Quest 2
* Meta Quest 3
