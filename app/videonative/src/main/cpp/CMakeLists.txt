
cmake_minimum_required(VERSION 3.6)

project("VideoNative")

include_directories(libs/include)

add_library(${CMAKE_PROJECT_NAME} SHARED
        parser/H26XParser.cpp
        parser/ParseRTP.cpp
        AudioDecoder.cpp
        UdpReceiver.cpp
        UdsReceiver.cpp
        VideoDecoder.cpp
        VideoPlayer.cpp)


target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        android
        mediandk
        aaudio
        ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libopus.so
        log)

set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY CXX_STANDARD 20)
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -fno-omit-frame-pointer)