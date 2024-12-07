//
// Created by Constantin on 2/6/2019.
//

#ifndef FPVUE_ANDROID_NALU_H
#define FPVUE_ANDROID_NALU_H

// https://github.com/Dash-Industry-Forum/Conformance-and-reference-source/blob/master/conformance/TSValidator/h264bitstream/h264_stream.h

#include <assert.h>
#include <array>
#include <chrono>
#include <cstdint>  // for uint8_t
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "NALUnitType.hpp"

// dependency could be easily removed again
#include <android/log.h>
#include <optional>
#include <variant>

#include "NALUnitType.hpp"

/**
 * NOTE: NALU only takes a c-style data pointer - it does not do any memory management. Use NALUBuffer if you need to
 * store a NALU. Since H264 and H265 are that similar, we use this class for both (make sure to not call methds only
 * supported on h265 with a h264 nalu,though) The constructor of the NALU does some really basic validation - make sure
 * the parser never produces a NALU where this validation would fail
 */
class NALU
{
  public:
    NALU(
        const uint8_t*                              data1,
        size_t                                      data_len1,
        const bool                                  IS_H265_PACKET1 = false,
        const std::chrono::steady_clock::time_point creationTime    = std::chrono::steady_clock::now())
        : m_data(data1), m_data_len(data_len1), IS_H265_PACKET(IS_H265_PACKET1), creationTime{creationTime}
    {
        assert(hasValidPrefix());
        assert(getSize() >= getMinimumNaluSize(IS_H265_PACKET1));
        m_nalu_prefix_size = get_nalu_prefix_size();
    }

    ~NALU() = default;

    // test video white iceland: Max 1024*117. Video might not be decodable if its NALU buffers size exceed the limit
    // But a buffer size of 1MB accounts for 60fps video of up to 60MB/s or 480 Mbit/s. That should be plenty !
    static constexpr const auto NALU_MAXLEN = 1024 * 1024;
    // Application should re-use NALU_BUFFER to avoid memory allocations
    using NALU_BUFFER = std::array<uint8_t, NALU_MAXLEN>;

  private:
    const uint8_t* m_data;
    const size_t   m_data_len;
    int            m_nalu_prefix_size;

  public:
    const bool IS_H265_PACKET;
    // creation time is used to measure latency
    const std::chrono::steady_clock::time_point creationTime;

  public:
    // returns true if starts with 0001, false otherwise
    bool hasValidPrefixLong() const { return m_data[0] == 0 && m_data[1] == 0 && m_data[2] == 0 && m_data[3] == 1; }

    // returns true if starts with 001 (short prefix), false otherwise
    bool hasValidPrefixShort() const { return m_data[0] == 0 && m_data[1] == 0 && m_data[2] == 1; }

    bool hasValidPrefix() const { return hasValidPrefixLong() || hasValidPrefixShort(); }

    int get_nalu_prefix_size() const
    {
        if (hasValidPrefixLong()) return 4;
        return 3;
    }

    static std::size_t getMinimumNaluSize(const bool isH265)
    {
        // 4 bytes prefix, 1 byte header for h264, 2 byte header for h265
        return isH265 ? 6 : 5;
    }

  public:
    // pointer to the NALU data with 0001 prefix
    const uint8_t* getData() const { return m_data; }

    // size of the NALU data with 0001 prefix
    size_t getSize() const { return m_data_len; }

    // pointer to the NALU data without 0001 prefix
    const uint8_t* getDataWithoutPrefix() const { return &getData()[m_nalu_prefix_size]; }

    // size of the NALU data without 0001 prefix
    ssize_t getDataSizeWithoutPrefix() const { return getSize() - m_nalu_prefix_size; }

    // return the nal unit type (quick)
    int get_nal_unit_type() const
    {
        if (IS_H265_PACKET)
        {
            return (getDataWithoutPrefix()[0] & 0x7E) >> 1;
        }
        return getDataWithoutPrefix()[0] & 0x1f;
    }

    std::string get_nal_unit_type_as_string() const
    {
        if (IS_H265_PACKET)
        {
            return NALUnitType::H265::unit_type_to_string(get_nal_unit_type());
        }
        return NALUnitType::H264::unit_type_to_string(get_nal_unit_type());
    }

  public:
    bool isSPS() const
    {
        if (IS_H265_PACKET)
        {
            return get_nal_unit_type() == NALUnitType::H265::NAL_UNIT_SPS;
        }
        return (get_nal_unit_type() == NALUnitType::H264::NAL_UNIT_TYPE_SPS);
    }

    bool isPPS() const
    {
        if (IS_H265_PACKET)
        {
            return get_nal_unit_type() == NALUnitType::H265::NAL_UNIT_PPS;
        }
        return (get_nal_unit_type() == NALUnitType::H264::NAL_UNIT_TYPE_PPS);
    }

    // VPS NALUs are only possible in H265
    bool isVPS() const
    {
        assert(IS_H265_PACKET);
        return get_nal_unit_type() == NALUnitType::H265::NAL_UNIT_VPS;
    }

    bool is_aud() const
    {
        if (IS_H265_PACKET)
        {
            return get_nal_unit_type() == NALUnitType::H265::NAL_UNIT_ACCESS_UNIT_DELIMITER;
        }
        return (get_nal_unit_type() == NALUnitType::H264::NAL_UNIT_TYPE_AUD);
    }

    bool is_sei() const
    {
        if (IS_H265_PACKET)
        {
            return get_nal_unit_type() == NALUnitType::H265::NAL_UNIT_PREFIX_SEI ||
                   get_nal_unit_type() == NALUnitType::H265::NAL_UNIT_SUFFIX_SEI;
        }
        return (get_nal_unit_type() == NALUnitType::H264::NAL_UNIT_TYPE_SEI);
    }

    bool is_dps() const
    {
        if (IS_H265_PACKET)
        {
            // doesn't exist in h265
            return false;
        }
        return (get_nal_unit_type() == NALUnitType::H264::NAL_UNIT_TYPE_DPS);
    }

    bool is_config() { return isSPS() || isPPS() || (IS_H265_PACKET && isVPS()); }

    // keyframe / IDR frame
    bool is_keyframe() const
    {
        const auto nut = get_nal_unit_type();
        if (IS_H265_PACKET)
        {
            return false;
        }
        if (nut == NALUnitType::H264::NAL_UNIT_TYPE_CODED_SLICE_IDR)
        {
            return true;
        }
        return false;
    }

    bool is_frame_but_not_keyframe() const
    {
        const auto nut = get_nal_unit_type();
        if (IS_H265_PACKET) return false;
        return (nut == NALUnitType::H264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR);
    }
    // XXX -----------

    std::string getDataAsHexString() const
    {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');

        for (size_t i = 0; i < getSize(); ++i)
        {
            ss << std::setw(2) << static_cast<int>(getData()[i]);
        }

        return ss.str();
    }

    // For debugging, return the whole NALU data as a big string for logging
    //   std::string dataAsString()const{
    //       return StringHelper::vectorAsString(std::vector<uint8_t>(getData(),getData()+getSize()));
    //   }
    //   void debug()const{
    //        if(IS_H265_PACKET){
    //            if(isSPS()){
    //                auto sps=h265nal::H265SpsParser::ParseSps(&getData()[6],getSize()-6);
    //                if(sps!=absl::nullopt){
    //                    MLOGD<<"SPS:"<<sps->dump();
    //                }else{
    //                    MLOGD<<"SPS parse error";
    //                }
    //            }else if(isPPS()){
    //                auto pps=h265nal::H265PpsParser::ParsePps(&getData()[6],getSize()-6);
    //                if(pps!=absl::nullopt){
    //                    MLOGD<<"PPS:"<<pps->dump();
    //                }else{
    //                    MLOGD<<"PPS parse error";
    //                }
    //            }
    //            else if(isVPS()){
    //                auto vps=h265nal::H265VpsParser::ParseVps(&getData()[6],getSize()-6);
    //                if(vps!=absl::nullopt){
    //                    MLOGD<<"VPS:"<<vps->dump();
    //                }else{
    //                    MLOGD<<"VPS parse error";
    //                }
    //            }else{
    //                MLOGD<<get_nal_unit_type_as_string();
    //            }
    //            return;
    //        }else{
    //            if(isSPS()){
    //                auto sps=H264::SPS(getData(),getSize());
    //                MLOGD<<"SPS:"<<sps.asString();
    //                //MLOGD<<"Has vui"<<sps.parsed.vui_parameters_present_flag;
    //                //MLOGD<<"SPS Latency:"<<H264Stream::latencyAffectingValues(&sps.parsed);
    //                MLOGD<<"SPSData:"<<StringHelper::vectorAsString(std::vector<uint8_t>(getData(),getData()+getSize()));
    //
    //                //H264::testSPSConversion(getData(),getSize());
    //                //RBSPHelper::test_unescape_escape(std::vector<uint8_t>(&getData()[5],&getData()[5]+getSize()-5));
    //            }else if(isPPS()){
    //                auto pps=H264::PPS(getData(),getSize());
    //                MLOGD<<"PPS:"<<pps.asString();
    //                MLOGD<<"PPSData:"<<StringHelper::vectorAsString(std::vector<uint8_t>(getData(),getData()+getSize()));
    //            }else if(is_aud()){
    //                MLOGD<<"AUD:"<<StringHelper::vectorAsString(std::vector<uint8_t>(getData(),getData()+getSize()));
    //            }else if(get_nal_unit_type()==NAL_UNIT_TYPE_SEI){
    //                MLOGD<<"SEIData:"<<StringHelper::vectorAsString(std::vector<uint8_t>(getData(),getData()+getSize()));
    //            }else{
    //                MLOGD<<get_nal_unit_type_as_string();
    //                if(get_nal_unit_type()==NAL_UNIT_TYPE_CODED_SLICE_IDR ||
    //                get_nal_unit_type()==NAL_UNIT_TYPE_CODED_SLICE_NON_IDR){
    //                    auto tmp=H264::Slice(getData(),getSize());
    //                    MLOGD<<"Slice header("<<StringHelper::memorySizeReadable(getSize())<<"):"<<tmp.asString();
    //                }
    //            }
    //        }
    //        //auto tmp=std::vector<uint8_t>(data,data+data_len);
    //        //MLOGD<<StringHelper::vectorAsString(tmp)<<" "<<tmp.size();
    //    }

    // Returns video width and height if the NALU is an SPS
    std::array<int, 2> getVideoWidthHeightSPS() const
    {
        assert(isSPS());
        if (IS_H265_PACKET)
        {
            return {1280, 720};
            //            auto sps=h265nal::H265SpsParser::ParseSps(&getData()[6],getSize()-6);
            //            if(sps!=absl::nullopt){
            //                std::array<int,2>
            //                ret={(int)sps->pic_width_in_luma_samples,(int)sps->pic_height_in_luma_samples}; return
            //                ret;
            //            }
            //            MLOGE<<"Couldn't parse h265 sps";
            //            return {640,480};
        }
        else
        {
            // const auto sps=H264::SPS(getData(),getSize());
            // return sps.getWidthHeightPx();
            return {640, 480};
        }
    }
    //
    // XXX -----------
};

typedef std::function<void(const NALU& nalu)> NALU_DATA_CALLBACK;

// Copies the nalu data into its own c++-style managed buffer.
class NALUBuffer
{
  public:
    NALUBuffer(const uint8_t* data, int data_len, bool is_h265, std::chrono::steady_clock::time_point creation_time)
    {
        m_data = std::make_shared<std::vector<uint8_t>>(data, data + data_len);
        m_nalu = std::make_unique<NALU>(m_data->data(), m_data->size(), is_h265, creation_time);
    }

    NALUBuffer(const NALU& nalu)
    {
        m_data = std::make_shared<std::vector<uint8_t>>(nalu.getData(), nalu.getData() + nalu.getSize());
        m_nalu = std::make_unique<NALU>(m_data->data(), m_data->size(), nalu.IS_H265_PACKET, nalu.creationTime);
    }

    NALUBuffer(const NALUBuffer&) = delete;

    NALUBuffer(const NALUBuffer&&) = delete;

    const NALU& get_nal() { return *m_nalu; }

  private:
    std::shared_ptr<std::vector<uint8_t>> m_data;
    std::unique_ptr<NALU>                 m_nalu;
};

#endif  // FPVUE_ANDROID_NALU_H
