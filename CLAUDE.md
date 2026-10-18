# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

PixelPilot is an Android FPV ground-station app (package `com.openipc.pixelpilot`). A USB Realtek
RTL8812AU/EU adapter is driven entirely in userspace (no kernel driver, no root) to receive a
wfb-ng (wifibroadcast) stream, which is decoded with MediaCodec and shown with an OSD. Nearly all
of the hot path is C++ behind thin Java JNI wrappers. Targets arm64-v8a and armeabi-v7a, minSdk 26.

## Build

Submodules are required (`devourer` and `wfb-ng` live under `app/wfbngrtl8812/src/main/cpp/`):

```
git submodule update --init --recursive
./gradlew assembleDebug            # app/build/outputs/apk/debug/app-debug.apk
./gradlew assembleRelease          # CI signs with -Pandroid.injected.signing.* (see .github/workflows/build.yml)
./gradlew :app:wfbngrtl8812:assembleDebug   # build a single native module
```

Toolchain: JDK 17, AGP 8.5.2, NDK 26.1.10909125, CMake 3.22.1. `org.gradle.configuration-cache=true`
is on. The release `signingConfig` references `../fpv.jks`, which is not in the repo; CI overrides
it with injected signing properties. `versionName`/`versionCode` are derived from git in
`app/build.gradle` (`git describe --tags` and the commit count); cut a release by pushing a `vX.Y.Z` tag.

Prebuilt native deps are checked in per ABI: `app/wfbngrtl8812/src/main/cpp/libs/<abi>/`
(libusb1.0, libsodium, libpcap) and `app/videonative/src/main/cpp/libs/<abi>/` (libopus).
devourer is compiled by listing its sources directly in `app/wfbngrtl8812/src/main/cpp/CMakeLists.txt`
(its own CMake needs pkg-config libusb). When bumping the devourer submodule, re-check that
source list against devourer's CMakeLists; chip generations are selected with
`DEVOURER_HAVE_JAGUAR1/JAGUAR3/JAGUAR3_8822E`.

## Formatting and tests

```
scripts/format_sources.sh          # clang-format (.clang-format, LLVM-based, 120 cols) over project C/C++,
                                   # skipping submodules, .cxx, app/mavlink and the vendored include dirs
pip install clang-format==18.1.8   # CI pins this exact version; other releases format differently
```

CI on pull requests (`.github/workflows/pr.yml`): format check, the host GoogleTest below, an
`assembleDebug` whose APK is uploaded as a `PixelPilot-debug-<sha>` artifact, and a non-blocking
emulator smoke job (API 34 x86_64 under ARM translation, driven by `flows/pixelpilot_smoke.py` from
OpenIPC/android-emulator-qa). Submodule checkout is deliberately non-recursive: devourer's
`reference/` submodules are not needed and one is unfetchable.

There are no Java/Android unit tests. The only native test is a host-side GoogleTest for
`BufferedPacketQueue`, built outside Gradle:

```
cd app/videonative/src/main/cpp/tests
cmake -B build && cmake --build build && ctest --test-dir build
# single test: ./build/queue_test --gtest_filter='Name.Case'
```

Real verification is on-device (or via the PR's debug APK artifact): install the APK, plug in a supported adapter (IDs in
`app/src/main/res/xml/usb_device_filter.xml`), watch logcat tags `pixelpilot` and `devourer`.

## Architecture

Four Gradle modules. The three library modules each contain one JNI `.so` whose Java side holds a
`long` pointer to a native instance and polls it with a `Timer` calling `nativeCallBack`
(there is no native-to-Java event queue):

| Module | Java entry | Native lib | Role |
|---|---|---|---|
| `app` | `VideoActivity` (single activity, ~1700 lines), `WfbLinkManager`, `WfbNgVpnService`, `osd/` | – | UI, settings (SharedPreferences `"general"`, `"osd_config"`), USB lifecycle, DVR, OSD |
| `app/wfbngrtl8812` | `WfbNgLink` | `libWfbngRtl8812.so` | USB wifi RX/TX via devourer + wfb-ng decrypt/FEC, adaptive link |
| `app/videonative` | `VideoPlayer` | `libVideoNative.so` | UDP/RTP receive, H.264/H.265 parse, MediaCodec decode, Opus audio, MP4 DVR |
| `app/mavlink` | `MavlinkNative` | `libmavlink.so` | MAVLink UDP listener feeding OSD telemetry |

### Data path (all loopback UDP on 127.0.0.1)

```
USB adapter --libusb(fd from UsbManager)--> devourer RX loop --> RxFrame channel-id match
   radio port 0x00  -> wfb-ng AggregatorUDPv4 -> :5600  -> videonative UdpReceiver -> decoder/DVR
   radio port 0x10  -> AggregatorUDPv4        -> :14550 -> mavlink.cpp -> OSDManager
   radio port 32    -> AggregatorUDPv4        -> :8000  -> WfbNgVpnService -> TUN 10.5.0.3/24
TUN 10.5.0.0/24 egress -> WfbNgVpnService -> :8001 -> TxFrame (FEC k=1,n=5, radio port 160) -> devourer TX -> air
```

- Modules do not call each other natively; they are coupled only through these ports and the
  `link_id` (7669206) plus radio-port constants in `WfbngLink.hpp`/`initAgg()`.
- `WfbNgVpnService` is the generic IP tunnel to the drone (drone side is 10.5.0.10). The embedded
  WebView and the adaptive-link feedback both go over it.
- Adaptive link: `start_link_quality_thread` sends a text status line to `10.5.0.10:9999` every
  100 ms. Its format is documented inline in `WfbngLink.cpp`; `FecChangeController` provides the
  decaying 0–5 `fec_change` value and the thresholds are user-tunable (README "Auto-FEC" section,
  `nativeSetFecThresholds`). `SignalQualityCalculator` is a singleton fed from the RX callback.
- `gs.key` is copied from assets (or a user-picked file) to the app files dir; native code opens
  it by the hard-coded path in `WfbngLink.hpp` (`/data/user/0/com.openipc.pixelpilot/files/gs.key`),
  so the applicationId cannot change without updating that constant. devourer's per-adapter lock
  file is pointed at the same dir because `/tmp` does not exist on Android.
- One `WfbNgLink.start()` per USB device spawns a thread running `nativeRun`, which blocks in
  devourer's `StartRxLoop` until `nativeStop` → `StopRxLoop`. Multi-adapter is supported by
  keying `rtl_devices` on the USB fd. Hot-plug is handled by `WfbLinkManager` (a
  `BroadcastReceiver`) diffing attached vs active adapters.
- Video can also arrive without an adapter: `videonative` listens on :5600 regardless, so any
  sender on the phone's Wi-Fi works (the UI shows the `udp://<ip>:5600` hint).

## Conventions

- Log tag is `"pixelpilot"` across Java and most C++; devourer logs under `"devourer"`.
- Qodo reviews every PR; its repo-level guidelines live in `.pr_agent.toml` and mirror the rules above.
- JNI symbols follow `Java_com_openipc_<module>_<Class>_<method>`. Release builds run R8:
  `app/proguard-rules.pro` keeps native-method classes and the mavlink types, and classes that
  native code reflects on (`WfbNGStats`, `DecodingInfo`, callback interfaces) are `@Keep`. Any new
  Java type touched from C++ needs one of those two protections or release builds crash.
- Native code is C++20 (`videonative` test harness is C++17). Keep `-fno-omit-frame-pointer`.
- `app/mavlink/src/main/cpp/mavlink/` is the generated MAVLink C library; do not reformat or hand-edit it.
