
cmake_minimum_required(VERSION 3.22.1)

project("WfbngRtl8812")

include_directories(include)

add_library(wfb-ng STATIC
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/fec.c
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/fec.h
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/ieee80211_radiotap.h
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/radiotap.c
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/rx.cpp
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/rx.hpp
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/wifibroadcast.hpp
        ${CMAKE_SOURCE_DIR}/wfb-ng/src/wifibroadcast.cpp)
set_target_properties(wfb-ng PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/wfb-ng)
target_include_directories(wfb-ng PUBLIC ${CMAKE_SOURCE_DIR}/wfb-ng)
target_compile_definitions(wfb-ng PRIVATE __WFB_RX_SHARED_LIBRARY__)

add_library(devourer STATIC
        ${CMAKE_SOURCE_DIR}/devourer/hal/Hal8812PhyReg.h
        ${CMAKE_SOURCE_DIR}/devourer/hal/Hal8812PwrSeq.c
        ${CMAKE_SOURCE_DIR}/devourer/hal/Hal8812PwrSeq.h
        ${CMAKE_SOURCE_DIR}/devourer/hal/basic_types.h
        ${CMAKE_SOURCE_DIR}/devourer/hal/hal8812a_fw.c
        ${CMAKE_SOURCE_DIR}/devourer/hal/hal8812a_fw.h
        ${CMAKE_SOURCE_DIR}/devourer/hal/hal_com_reg.h
        ${CMAKE_SOURCE_DIR}/devourer/hal/rtl8812a_hal.h
        ${CMAKE_SOURCE_DIR}/devourer/hal/rtl8812a_recv.h
        ${CMAKE_SOURCE_DIR}/devourer/hal/rtl8812a_spec.h

        ${CMAKE_SOURCE_DIR}/devourer/src/EepromManager.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/EepromManager.h
        ${CMAKE_SOURCE_DIR}/devourer/src/Firmware.h
        ${CMAKE_SOURCE_DIR}/devourer/src/FirmwareManager.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/FirmwareManager.h
        ${CMAKE_SOURCE_DIR}/devourer/src/FrameParser.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/FrameParser.h
        ${CMAKE_SOURCE_DIR}/devourer/src/HalModule.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/HalModule.h
        ${CMAKE_SOURCE_DIR}/devourer/src/ParsedRadioPacket.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/RadioManagementModule.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/RadioManagementModule.h
        ${CMAKE_SOURCE_DIR}/devourer/src/Rtl8812aDevice.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/Rtl8812aDevice.h
        ${CMAKE_SOURCE_DIR}/devourer/src/RtlUsbAdapter.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/RtlUsbAdapter.h
        ${CMAKE_SOURCE_DIR}/devourer/src/SelectedChannel.h
        ${CMAKE_SOURCE_DIR}/devourer/src/WiFiDriver.cpp
        ${CMAKE_SOURCE_DIR}/devourer/src/WiFiDriver.h
        ${CMAKE_SOURCE_DIR}/devourer/src/registry_priv.h)
set_target_properties(devourer PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/devourer)
target_include_directories(devourer PUBLIC ${CMAKE_SOURCE_DIR}/devourer)
target_include_directories(devourer PUBLIC ${CMAKE_SOURCE_DIR}/devourer/hal)
set_property(TARGET devourer PROPERTY CXX_STANDARD 20)

# WFB-NG RTL8812 library
add_library(${CMAKE_PROJECT_NAME} SHARED
        RxFrame.h
        RxFrame.cpp
        WfbngLink.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
        devourer
        wfb-ng
        android
        log
        ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libusb1.0.so
        ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libsodium.so
        ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libpcap.a)

set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY CXX_STANDARD 20)
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -fno-omit-frame-pointer)