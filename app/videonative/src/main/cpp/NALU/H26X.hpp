//
// Created by consti10 on 04.11.20.
//

#ifndef FPVUE_H264_H
#define FPVUE_H264_H

#include "NALUnitType.hpp"

// namespaces for H264 H265 helper
// A H265 NALU is kind of similar to a H264 NALU in that it has the same [0,0,0,1] prefix

namespace RBSPHelper {
    // The rbsp buffer starts after 5 bytes for h264 (4 bytes prefix and 1 byte unit header)
    // and after 6 bytes for h265 (4 bytes prefix and 2 byte unit header)
    // the h264bitstream nal_to_rbsp function is buggy !
    // Also, its implementation unescapes the NAL unit header which is wrong (only the payload should be unescaped)
    // escaping/unescaping rbsp is the same for h264 and h265

    static std::vector<uint8_t>
    unescapeRbsp(const uint8_t *rbsp_buff, const std::size_t rbsp_buff_size) {
        auto ret = h265nal::UnescapeRbsp(rbsp_buff, rbsp_buff_size);
        return ret;
    }

    static std::vector<uint8_t> unescapeRbsp(const std::vector<uint8_t> &rbspData) {
        return unescapeRbsp(rbspData.data(), rbspData.size());
    }

    static std::vector<uint8_t> escapeRbsp(const std::vector<uint8_t> &rbspBuff) {
        std::vector<uint8_t> rbspBuffEscaped;
        rbspBuffEscaped.resize(rbspBuff.size() + 32);
        int rbspSize = rbspBuff.size();
        int rbspBuffEscapedSize = rbspBuffEscaped.size();
        auto tmp = rbsp_to_nal(rbspBuff.data(), &rbspSize, rbspBuffEscaped.data(),
                               &rbspBuffEscapedSize);
        assert(tmp > 0);
        rbspBuffEscaped.resize(tmp);
        // workaround h264bitstream library
        return std::vector<uint8_t>(&rbspBuffEscaped.data()[1],
                                    &rbspBuffEscaped.data()[1] + rbspBuffEscaped.size() - 1);
    }

    static void test_unescape_escape(const std::vector<uint8_t> &rbspDataEscaped) {
        auto unescapedData = unescapeRbsp(rbspDataEscaped.data(), rbspDataEscaped.size());
        auto unescapedThenEscapedData = escapeRbsp(unescapedData);
        MLOGD << "Y1:" << StringHelper::vectorAsString(rbspDataEscaped);
        MLOGD << "Y2:" << StringHelper::vectorAsString(unescapedData);
        MLOGD << "Y3:" << StringHelper::vectorAsString(unescapedThenEscapedData);
        // check if the data stayed the same after unescaping then escaping it again
        assert(rbspDataEscaped.size() == unescapedThenEscapedData.size());
        assert(std::memcmp(rbspDataEscaped.data(), unescapedThenEscapedData.data(),
                           rbspDataEscaped.size()) == 0);
    }
}

namespace H264 {
    // reverse order due to architecture
    // The nal unit header is not part of the rbsp-escaped bitstream and therefore can be read without unescaping anything
    typedef struct nal_unit_header {
        uint8_t nal_unit_type: 5;
        uint8_t nal_ref_idc: 2;
        uint8_t forbidden_zero_bit: 1;

        std::string asString() const {
            std::stringstream ss;
            ss << "nal_unit_type:" << (int) nal_unit_type << " nal_ref_idc:" << (int) nal_ref_idc
               << " forbidden_zero_bit:" << (int) forbidden_zero_bit;
            return ss.str();
        }
    }__attribute__ ((packed)) nal_unit_header_t;
    static_assert(sizeof(nal_unit_header_t) == 1);

    // Parse raw NALU data into an sps struct (using the h264bitstream library)
    class SPS {
    public:
        nal_unit_header_t nal_header;
        sps_t parsed;
    public:
        // data buffer= NALU data with prefix
        SPS(const uint8_t *nalu_data, size_t data_len) {
            memcpy(&nal_header, &nalu_data[4], 1);
            assert(nal_header.forbidden_zero_bit == 0);
            assert(nal_header.nal_unit_type == NAL_UNIT_TYPE_SPS);
            auto rbsp_buf = RBSPHelper::unescapeRbsp(&nalu_data[5], data_len - 5);
            BitStream b(rbsp_buf);
            read_seq_parameter_set_rbsp(&parsed, b.bs_t());
            read_rbsp_trailing_bits(b.bs_t());
        }

        std::array<int, 2> getWidthHeightPx() const {
            int Width = ((parsed.pic_width_in_mbs_minus1 + 1) * 16) -
                        parsed.frame_crop_right_offset * 2 - parsed.frame_crop_left_offset * 2;
            int Height = ((2 - parsed.frame_mbs_only_flag) *
                          (parsed.pic_height_in_map_units_minus1 + 1) * 16) -
                         (parsed.frame_crop_bottom_offset * 2) - (parsed.frame_crop_top_offset * 2);
            return {Width, Height};
        }

        std::string asString() const {
            return H264Stream::spsAsString(&parsed);
        }

// --------------------------------------------- crude hacking --------------------------------------------
        std::vector<uint8_t> asNALU() const {
            std::vector<uint8_t> naluBuff;
            // write prefix and nalu header
            naluBuff.resize(5);
            naluBuff[0] = 0;
            naluBuff[1] = 0;
            naluBuff[2] = 0;
            naluBuff[3] = 1;
            std::memcpy(&naluBuff.data()[4], &nal_header, 1);
            // write sps payload into rbspBuff
            std::vector<uint8_t> rbspBuff;
            rbspBuff.resize(14 + 64 * 4);
            //MLOGD<<"rbspbuffSize"<<rbspBuff.size();
            BitStream b(rbspBuff);
            //
            write_seq_parameter_set_rbsp(&parsed, b.bs_t());
            write_rbsp_trailing_bits(b.bs_t());

            if (bs_overrun(b.bs_t())) {
                MLOGE << "BS overrun ";
            }
            auto rbsp_size = bs_pos(b.bs_t());
            rbspBuff.resize(rbsp_size);
            // escape sps payload and append to nalu buffer
            auto rbspBuffEscaped = RBSPHelper::escapeRbsp(rbspBuff);
            naluBuff.insert(naluBuff.end(), rbspBuffEscaped.begin(), rbspBuffEscaped.end());
            return naluBuff;
        }

        //parsed.pic_order_cnt_type=0;
        //parsed.log2_max_pic_order_cnt_lsb_minus4=4;
        void experiment() {
            //parsed.level_idc=40;
            //parsed.pic_order_cnt_type=2;
            //parsed.log2_max_pic_order_cnt_lsb_minus4=4;
        }

        // adding VUI might decrease latency for some streams, if max_dec_frame_buffering is set properly
        // https://community.intel.com/t5/Media-Intel-oneAPI-Video/h-264-decoder-gives-two-frames-latency-while-decoding-a-stream/td-p/1099694
        void addVUI() {
            //parsed.level_idc=40;
            //parsed.constraint_set4_flag=true;
            //parsed.constraint_set5_flag=true;
            //parsed.log2_max_frame_num_minus4=12;
            //parsed.log2_max_pic_order_cnt_lsb_minus4=12;

            parsed.vui_parameters_present_flag = 1;
            parsed.vui.aspect_ratio_info_present_flag = 0;
            parsed.vui.aspect_ratio_idc = 0;
            parsed.vui.sar_width = 0;
            parsed.vui.sar_height = 0;
            parsed.vui.overscan_info_present_flag = 0;
            parsed.vui.overscan_appropriate_flag = 0;
            parsed.vui.video_signal_type_present_flag = 1;
            parsed.vui.video_format = 5;
            parsed.vui.video_full_range_flag = 0;
            parsed.vui.colour_description_present_flag = 1;
            parsed.vui.colour_primaries = 0;
            parsed.vui.transfer_characteristics = 0;
            parsed.vui.matrix_coefficients = 5;
            parsed.vui.chroma_loc_info_present_flag = 0;
            parsed.vui.chroma_sample_loc_type_top_field = 0;
            parsed.vui.chroma_sample_loc_type_bottom_field = 0;
            parsed.vui.timing_info_present_flag = 1;
            parsed.vui.num_units_in_tick = 1;
            parsed.vui.time_scale = 120;
            parsed.vui.fixed_frame_rate_flag = 1;
            parsed.vui.nal_hrd_parameters_present_flag = 0;
            parsed.vui.vcl_hrd_parameters_present_flag = 0;
            parsed.vui.low_delay_hrd_flag = 0;
            parsed.vui.pic_struct_present_flag = 0;
            parsed.vui.bitstream_restriction_flag = 1;
            parsed.vui.motion_vectors_over_pic_boundaries_flag = 1;
            parsed.vui.max_bytes_per_pic_denom = 0;
            parsed.vui.max_bits_per_mb_denom = 0;
            parsed.vui.log2_max_mv_length_horizontal = 16;
            parsed.vui.log2_max_mv_length_vertical = 16;
            parsed.vui.num_reorder_frames = 0;
            parsed.vui.max_dec_frame_buffering = 1;
        }
    };

    static void testSPSConversion(const uint8_t *nalu_data, size_t data_len) {
        auto sps = H264::SPS(nalu_data, data_len);
        auto naluBuff = sps.asNALU();
        MLOGD << "Z1:" << StringHelper::vectorAsString(
                std::vector<uint8_t>(nalu_data, nalu_data + data_len));
        MLOGD << "Z2:" << StringHelper::vectorAsString(naluBuff);
        assert(data_len == naluBuff.size());
        assert(std::memcmp(nalu_data, naluBuff.data(), data_len) == 0);
    }
// --------------------------------------------- crude hacking --------------------------------------------

    class PPS {
    public:
        nal_unit_header_t nal_header;
        pps_t parsed;
    public:
        // data buffer= NALU data with prefix
        PPS(const uint8_t *nalu_data, size_t data_len) {
            memcpy(&nal_header, &nalu_data[4], 1);
            assert(nal_header.forbidden_zero_bit == 0);
            assert(nal_header.nal_unit_type == NAL_UNIT_TYPE_PPS);
            auto rbsp_buf = RBSPHelper::unescapeRbsp(&nalu_data[5], data_len - 5);
            BitStream b(rbsp_buf);
            read_pic_parameter_set_rbsp(&parsed, b.bs_t());
            read_rbsp_trailing_bits(b.bs_t());
        }

        std::string asString() const {
            return H264Stream::ppsAsString(parsed);
        }
    };

    class Slice {
    public:
        nal_unit_header_t nal_header;
        slice_header_t parsed;
    public:
        // data buffer= NALU data with prefix
        Slice(const uint8_t *nalu_data, size_t data_len) {
            memcpy(&nal_header, &nalu_data[4], 1);
            assert(nal_header.forbidden_zero_bit == 0);
            assert(nal_header.nal_unit_type == NAL_UNIT_TYPE_CODED_SLICE_IDR ||
                   nal_header.nal_unit_type == NAL_UNIT_TYPE_CODED_SLICE_NON_IDR);
            auto rbsp_buf = RBSPHelper::unescapeRbsp(&nalu_data[5], data_len - 5);
            BitStream b(rbsp_buf);
            // parsing
            parsed.first_mb_in_slice = b.read_ue();
            parsed.slice_type = b.read_ue();
            parsed.pic_parameter_set_id = b.read_ue();
            parsed.frame_num = b.read_ue();
        }

        std::string asString() const {
            std::stringstream ss;
            ss << "[first_mb_in_slice=" << parsed.first_mb_in_slice << ",slice_type="
               << parsed.slice_type << ",pic_parameter_set_id=" << parsed.pic_parameter_set_id
               << "frame_num=" << parsed.frame_num << "]";
            return ss.str();
        }
    };

    /*class SEI{
    public:
        nal_unit_header_t nal_header;
        slice_header_t parsed;
    public:
        // data buffer= NALU data with prefix
        Slice(const uint8_t* nalu_data,size_t data_len){
            memcpy(&nal_header,&nalu_data[4],1);
            assert(nal_header.forbidden_zero_bit==0);
            assert(nal_header.nal_unit_type==NAL_UNIT_TYPE_CODED_SLICE_IDR || nal_header.nal_unit_type==NAL_UNIT_TYPE_CODED_SLICE_NON_IDR);
            auto rbsp_buf= RBSPHelper::unescapeRbsp(&nalu_data[5], data_len-5);
            BitStream b(rbsp_buf);
            // parsing
            parsed.first_mb_in_slice=b.read_ue();
            parsed.slice_type=b.read_ue();
            parsed.pic_parameter_set_id=b.read_ue();
            parsed.frame_num=b.read_ue();
        }
        std::string asString()const{
            std::stringstream ss;
            return ss.str();
        }
    };*/

    // this is the data for an h264 AUD unit
    static std::array<uint8_t, 6> EXAMPLE_AUD = {
            0, 0, 0, 1, 9, 48
    };
    static std::array<uint8_t, 687> EXAMPLE_SEI = {
            0, 0, 0, 1, 6, 5, 255, 255, 167, 220, 69, 233, 189, 230, 217, 72, 183, 150, 44, 216, 32,
            217, 35, 238, 239, 120, 50, 54, 52, 32, 45, 32, 99, 111, 114, 101, 32, 49, 52, 56, 32,
            45, 32, 72, 46, 50, 54, 52, 47, 77, 80, 69, 71, 45, 52, 32, 65, 86, 67, 32, 99, 111,
            100, 101, 99, 32, 45, 32, 67, 111, 112, 121, 108, 101, 102, 116, 32, 50, 48, 48, 51, 45,
            50, 48, 49, 54, 32, 45, 32, 104, 116, 116, 112, 58, 47, 47, 119, 119, 119, 46, 118, 105,
            100, 101, 111, 108, 97, 110, 46, 111, 114, 103, 47, 120, 50, 54, 52, 46, 104, 116, 109,
            108, 32, 45, 32, 111, 112, 116, 105, 111, 110, 115, 58, 32, 99, 97, 98, 97, 99, 61, 48,
            32, 114, 101, 102, 61, 51, 32, 100, 101, 98, 108, 111, 99, 107, 61, 49, 58, 48, 58, 48,
            32, 97, 110, 97, 108, 121, 115, 101, 61, 48, 120, 49, 58, 48, 120, 49, 49, 49, 32, 109,
            101, 61, 104, 101, 120, 32, 115, 117, 98, 109, 101, 61, 55, 32, 112, 115, 121, 61, 49,
            32, 112, 115, 121, 95, 114, 100, 61, 49, 46, 48, 48, 58, 48, 46, 48, 48, 32, 109, 105,
            120, 101, 100, 95, 114, 101, 102, 61, 49, 32, 109, 101, 95, 114, 97, 110, 103, 101, 61,
            49, 54, 32, 99, 104, 114, 111, 109, 97, 95, 109, 101, 61, 49, 32, 116, 114, 101, 108,
            108, 105, 115, 61, 49, 32, 56, 120, 56, 100, 99, 116, 61, 48, 32, 99, 113, 109, 61, 48,
            32, 100, 101, 97, 100, 122, 111, 110, 101, 61, 50, 49, 44, 49, 49, 32, 102, 97, 115,
            116,
            5, 112, 115, 107, 105, 112, 61, 49, 32, 99, 104, 114, 111, 109, 97, 95, 113, 112, 95,
            111, 102, 102, 115, 101, 116, 61, 45, 50, 32, 116, 104, 114, 101, 97, 100, 115, 61, 54,
            32, 108, 111, 111, 107, 97, 104, 101, 97, 100, 95, 116, 104, 114, 101, 97, 100, 115, 61,
            49, 32, 115, 108, 105, 99, 101, 100, 95, 116, 104, 114, 101, 97, 100, 115, 61, 48, 32,
            110, 114, 61, 48, 32, 100, 101, 99, 105, 109, 97, 116, 101, 61, 49, 32, 105, 110, 116,
            101, 114, 108, 97, 99, 101, 100, 61, 48, 32, 98, 108, 117, 114, 97, 121, 95, 99, 111,
            109, 112, 97, 116, 61, 48, 32, 99, 111, 110, 115, 116, 114, 97, 105, 110, 101, 100, 95,
            105, 110, 116, 114, 97, 61, 48, 32, 98, 102, 114, 97, 109, 101, 115, 61, 48, 32, 119,
            101, 105, 103, 104, 116, 112, 61, 48, 32, 107, 101, 121, 105, 110, 116, 61, 50, 57, 57,
            32, 107, 101, 121, 105, 110, 116, 95, 109, 105, 110, 61, 50, 57, 32, 115, 99, 101, 110,
            101, 99, 117, 116, 61, 52, 48, 32, 105, 110, 116, 114, 97, 95, 114, 101, 102, 114, 101,
            115, 104, 61, 48, 32, 114, 99, 95, 108, 111, 111, 107, 97, 104, 101, 97, 100, 61, 52,
            48, 32, 114, 99, 61, 99, 98, 114, 32, 109, 98, 116, 114, 101, 101, 61, 49, 32, 98, 105,
            116, 114, 97, 116, 101, 61, 50, 48, 52, 56, 32, 114, 97, 116, 101, 116, 111, 108, 61,
            49, 46, 48, 32, 113, 99, 111, 109, 112, 61, 48, 46, 54, 48, 32, 113, 112, 109, 105, 110,
            1, 48, 32, 113, 112, 109, 97, 120, 61, 54, 57, 32, 113, 112, 115, 116, 101, 112, 61, 52,
            32, 118, 98, 118, 95, 109, 97, 120, 114, 97, 116, 101, 61, 50, 48, 52, 56, 32, 118, 98,
            118, 95, 98, 117, 102, 115, 105, 122, 101, 61, 49, 50, 50, 56, 32, 110, 97, 108, 95,
            104, 114, 100, 61, 110, 111, 110, 101, 32, 102, 105, 108, 108, 101, 114, 61, 48, 32,
            105, 112, 95, 114, 97, 116, 105, 111, 61, 49, 46, 52, 48, 32, 97, 113, 61, 49, 58, 49,
            46, 48, 48, 0, 128
    };

}

namespace H265 {
    typedef struct nal_unit_header {
        uint8_t forbidden_zero_bit: 1;
        uint8_t nal_unit_type: 6;
        uint8_t nuh_layer_id: 6;
        uint8_t nuh_temporal_id_plus1: 3;
    }__attribute__ ((packed)) nal_unit_header_t;
    static_assert(sizeof(nal_unit_header_t) == 2);

}

#endif //FPVUE_H264_H
