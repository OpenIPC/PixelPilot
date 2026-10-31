//
// Created by Constantin on 24.01.2018.
//
#include "H26XParser.h"
#include <android/log.h>
#include <endian.h>
#include <chrono>
#include <cstring>
#include <thread>

H26XParser::H26XParser(NALU_DATA_CALLBACK onNewNALU)
    : onNewNALU(std::move(onNewNALU)),
      mDecodeRTP(std::bind(
          &H26XParser::onNewNaluDataExtracted,
          this,
          std::placeholders::_1,
          std::placeholders::_2,
          std::placeholders::_3))
{
}

void H26XParser::reset()
{
    mDecodeRTP.reset();
    mNewestSeq                 = -1;
    nParsedNALUs               = 0;
    nParsedKonfigurationFrames = 0;
}

void H26XParser::parse_rtp_stream(const uint8_t* rtp_data, const size_t data_length)
{
    const RTP::RTPPacket rtpPacket(rtp_data, data_length);
    bool                 accepted = false;
    if (rtpPacket.header.payload == RTP_PAYLOAD_TYPE_H264)
    {
        IS_H265  = false;
        accepted = mDecodeRTP.parseRTPH264toNALU(rtp_data, data_length);
    }
    else if (rtpPacket.header.payload == RTP_PAYLOAD_TYPE_H265)
    {
        IS_H265  = true;
        accepted = mDecodeRTP.parseRTPH265toNALU(rtp_data, data_length);
    }
    if (!accepted)
    {
        return;
    }
    // A marker ends the current access unit only from a packet the RTP decoder
    // took and that is newer than any seen so far: a late copy of the previous
    // frame's last packet, or a second copy of the newest one, must not cut the
    // current frame short. A jump far behind is a restarted sender: resync.
    const uint16_t seq    = rtpPacket.header.getSequence();
    bool           newest = true;
    if (mNewestSeq >= 0)
    {
        const uint16_t ahead = static_cast<uint16_t>(seq - mNewestSeq);
        newest = ahead != 0 && (ahead < 0x8000 || static_cast<uint16_t>(mNewestSeq - seq) > kSeqResyncWindow);
    }
    if (newest)
    {
        mNewestSeq = seq;
    }
    if (rtpPacket.header.marker && newest && onAccessUnitEnd)
    {
        onAccessUnitEnd();
    }
}

void H26XParser::onNewNaluDataExtracted(
    const std::chrono::steady_clock::time_point creation_time, const uint8_t* nalu_data, const int nalu_data_size)
{
    NALU nalu(nalu_data, nalu_data_size, IS_H265, creation_time);
    newNaluExtracted(nalu);
}

void H26XParser::newNaluExtracted(const NALU& nalu)
{
    if (onNewNALU != nullptr)
    {
        onNewNALU(nalu);
    }
    nParsedNALUs++;
    const bool sps_or_pps = nalu.isSPS() || nalu.isPPS();
    if (sps_or_pps)
    {
        nParsedKonfigurationFrames++;
    }
}
