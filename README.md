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
- [devourer](https://github.com/openipc/devourer): userspace rtl8812au driver initially created by [buldo](https://github.com/buldo) and converted to C by [josephnef](https://github.com/josephnef).
- [LiveVideo10ms](https://github.com/Consti10/LiveVideo10ms): excellent video decoder from [Consti10](https://github.com/Consti10) converted into a module.
- [wfb-ng](https://github.com/svpcom/wfb-ng): library allowing the broadcast of the video feed over the air.

The wfb-ng [gs.key](https://github.com/OpenIPC/PixelPilot/raw/main/app/src/main/assets/gs.key) is embedded in the app.
The settings menu allows selecting a different key from your phone.

Supported rtl8812au wifi adapter are listed [here](https://github.com/OpenIPC/PixelPilot/blob/master/app/src/main/res/xml/usb_device_filter.xml).
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
