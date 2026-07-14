
cmake_minimum_required(VERSION 3.22.1)

project("WfbngRtl8812")

include_directories(include)

add_library(wfb-ng STATIC
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/zfex.c
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/zfex.h
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/ieee80211_radiotap.h
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/radiotap.c
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/rx.cpp
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/rx.hpp
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/wifibroadcast.hpp
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/wifibroadcast.cpp)
set_target_properties(wfb-ng PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/wfb-ng)
target_include_directories(wfb-ng PUBLIC ${CMAKE_SOURCE_DIR}/wfb-ng)
target_compile_definitions(wfb-ng PRIVATE
        __WFB_RX_SHARED_LIBRARY__
        PREINCLUDE_FILE=<${CMAKE_SOURCE_DIR}/wfb_log.h>
        ZFEX_UNROLL_ADDMUL_SIMD=8
        ZFEX_USE_INTEL_SSSE3
        ZFEX_USE_ARM_NEON
        ZFEX_INLINE_ADDMUL
        ZFEX_INLINE_ADDMUL_SIMD
)

# devourer — userspace Realtek 11ac driver. Mirrors the source groups of
# devourer's own CMakeLists.txt (which we can't add_subdirectory here because
# it resolves libusb via pkg-config, unavailable in the NDK build).
# Chip support compiled in:
#   Jaguar1 (RTL8812AU / 8811AU / 8821AU) — the classic PixelPilot adapters
#   Jaguar3 8822E (RTL8812EU / RTL8822EU) — e.g. LB-LINK BL-M8812EU2
add_library(devourer STATIC
        # --- generation-agnostic core ---
        ${CMAKE_SOURCE_DIR}/devourer/src/ParsedRadioPacket.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/Radiotap.c
        ${CMAKE_SOURCE_DIR}/devourer/src/RadiotapBuilder.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/RtlAdapter.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/UsbTransport.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/UsbDeviceLock.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/UsbOpen.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/TxMode.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/TxPower.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/LinkHealth.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/SignalStop.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/WiFiDriver.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/PhyTableLoader.cpp

        # --- Jaguar1 (RTL8812AU / 8811AU / 8821AU) ---
        ${CMAKE_SOURCE_DIR}/devourer/hal/hal8812a_fw.c
        ${CMAKE_SOURCE_DIR}/devourer/hal/hal8821a_fw.c
        ${CMAKE_SOURCE_DIR}/devourer/hal/Hal8812PwrSeq.c
        ${CMAKE_SOURCE_DIR}/devourer/hal/Hal8821APwrSeq.c
        ${CMAKE_SOURCE_DIR}/devourer/hal/Hal8812a_TxPwrTrack.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/RtlJaguarDevice.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/HalModule.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/RadioManagementModule.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/EepromManager.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/FirmwareManager.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/Iqk8812a.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/PhydmWatchdog.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/PowerTracking8812a.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/FrameParser.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar1/BbDbgportReader.cpp

        # --- Jaguar3 shared (dispatchers compiled for any Jaguar3 variant) ---
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/RtlJaguar3Device.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/HalJaguar3.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/HalmacJaguar3Fw.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/HalmacJaguar3MacInit.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/RadioManagementJaguar3.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/PhyTableLoaderJaguar3.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/Phy8822cTables.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/Halrf8822c.cpp

        # --- Jaguar3 8822E variant (RTL8812EU / RTL8822EU) ---
        ${CMAKE_SOURCE_DIR}/devourer/hal/hal8822e_fw.c
        ${CMAKE_SOURCE_DIR}/devourer/hal/phydm/rtl8822e/Hal8822e_PhyTables.c
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/Phy8822eTables.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/jaguar3/Halrf8822e.cpp)
set_target_properties(devourer PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/devourer)

target_compile_definitions(devourer PUBLIC
        DEVOURER_HAVE_JAGUAR1=1
        DEVOURER_HAVE_JAGUAR3=1
        DEVOURER_HAVE_JAGUAR3_8822E=1)

target_include_directories(devourer PUBLIC ${CMAKE_SOURCE_DIR}/devourer)
target_include_directories(devourer PUBLIC ${CMAKE_SOURCE_DIR}/devourer/src)
target_include_directories(devourer PUBLIC ${CMAKE_SOURCE_DIR}/devourer/hal)
target_include_directories(devourer PUBLIC ${CMAKE_SOURCE_DIR}/devourer/hal/phydm/rtl8822c)
target_include_directories(devourer PUBLIC ${CMAKE_SOURCE_DIR}/devourer/hal/phydm/rtl8822e)
target_link_libraries(devourer PUBLIC log)
set_property(TARGET devourer PROPERTY CXX_STANDARD 20)

# WFB-NG RTL8812 library
add_library(${CMAKE_PROJECT_NAME} SHARED
        RxFrame.h
        RxFrame.cpp
        WfbngLink.cpp
        TxFrame.h
        TxFrame.cpp
        SignalQualityCalculator.h
        SignalQualityCalculator.cpp
        )

target_link_libraries(${CMAKE_PROJECT_NAME}
        devourer
        wfb-ng
        android
        log
        ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libusb1.0.so
        ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libsodium.so
        ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libpcap.a)

set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY CXX_STANDARD 20)
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -fno-omit-frame-pointer )
