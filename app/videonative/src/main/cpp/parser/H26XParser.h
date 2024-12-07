//
// Created by Constantin on 24.01.2018.
//

#ifndef FPV_VR_PARSE2H264RAW_H
#define FPV_VR_PARSE2H264RAW_H

#include <functional>
#include <sstream>

/**
 * Input:
 * 1) rtp packets (h264/h265)
 * 2) raw packets (h264/h265)
 * Output:
 * NAL units in the onNewNalu callback, one after another
 */
//

#include "../NALU/NALU.hpp"

#include "ParseRTP.h"

//
#include <list>
#include <map>

class H26XParser
{
  public:
    H26XParser(NALU_DATA_CALLBACK onNewNALU);

    void parse_rtp_stream(const uint8_t* rtp_data, const size_t data_len);

    void reset();

  public:
    long nParsedNALUs               = 0;
    long nParsedKonfigurationFrames = 0;

    // For live video set to -1 (no fps limitation), else additional latency will be generated
    void setLimitFPS(int maxFPS);

  private:
    void newNaluExtracted(const NALU& nalu);

    void onNewNaluDataExtracted(
        const std::chrono::steady_clock::time_point creation_time, const uint8_t* nalu_data, const int nalu_data_size);

    const NALU_DATA_CALLBACK              onNewNALU;
    std::chrono::steady_clock::time_point lastFrameLimitFPS       = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastTimeOnNewNALUCalled = std::chrono::steady_clock::now();

    RTPDecoder mDecodeRTP;

    int  maxFPS  = 0;
    bool IS_H265 = false;
    // First time a NALU was succesfully decoded
    // std::chrono::steady_clock::time_point timeFirstNALUArrived=std::chrono::steady_clock::time_point(0);
};

#endif  // FPV_VR_PARSE2H264RAW_H
