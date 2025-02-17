//
// Created by gaeta on 2024-03-31.
//

#ifndef LIBUSBDEMO_RXFRAME_H
#define LIBUSBDEMO_RXFRAME_H

#include <array>
#include <iomanip>
#include <iostream>
#include <span>
#include <vector>

enum class RadioPort { /* define your RadioPort enum */ };

class RxFrame {
  private:
    std::span<uint8_t> _data;
    static constexpr std::array<uint8_t, 2> _dataHeader = {uint8_t(0x08),
                                                           uint8_t(0x01)}; // Frame control value for QoS Data

  public:
    RxFrame(const std::span<uint8_t> &data) : _data(data) { DataAsMemory = _data; }

    std::span<uint8_t> DataAsMemory; // useless in c++

    std::span<uint8_t> ControlField() const { return {_data.data(), 2}; }

    std::span<uint8_t> Duration() const { return {_data.data() + 2, 2}; }

    std::span<uint8_t> MacAp() const { return {_data.data() + 4, 6}; }               // receiverAddress
    std::span<uint8_t> MacSrcUniqueIdPart() const { return {_data.data() + 10, 1}; } // transmitterAddress
    std::span<uint8_t> MacSrcNoncePart1() const { return {_data.data() + 11, 4}; }

    std::span<uint8_t> MacSrcRadioPort() const { return {_data.data() + 15, 1}; }

    std::span<uint8_t> MacDstUniqueIdPart() const { return {_data.data() + 16, 1}; } // destinationAddress
    std::span<uint8_t> MacDstNoncePart2() const { return {_data.data() + 17, 4}; }

    std::span<uint8_t> MacDstRadioPort() const { return {_data.data() + 21, 1}; }

    std::span<uint8_t> SequenceControl() const { return {_data.data() + 22, 2}; }

    std::span<uint8_t> PayloadSpan() const { return {_data.data() + 24, _data.size() - 28}; }

    std::span<uint8_t> GetNonce() const {
        std::array<uint8_t, 8> data;
        std::copy(_data.begin() + 11, _data.begin() + 15, data.begin());
        std::copy(_data.begin() + 17, _data.begin() + 21, data.begin() + 4);
        return {data.data(), data.size()};
    }

    //    RadioPort get_valid_radio_port() const {
    //        return RadioPort::Fromuint8_t(_data[15]);
    //    }

    bool IsValidWfbFrame() const {
        if (_data.empty()) return false;
        if (!IsDataFrame()) return false;
        if (PayloadSpan().empty()) return false;
        if (!HasValidAirGndId()) return false;
        if (!HasValidRadioPort()) return false;
        // TODO: add `frame.PayloadSpan().size() > RAW_WIFI_FRAME_MAX_PAYLOAD_SIZE`
        return true;
    }

    uint8_t GetValidAirGndId() const { return _data[10]; }

    void printChannelId(std::ostream &ss) const {
        ss << "Channel ID: ";
        for (int i = 10; i < 22; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)_data[i];
        }
    }

    bool MatchesChannelID(const uint8_t *channel_id) const {
        //        0x57, 0x42, 0xaa, 0xbb, 0xcc, 0xdd,   // last four bytes are replaced by channel_id (x2)
        return _data[10] == 0x57 && _data[11] == 0x42 && _data[12] == channel_id[0] && _data[13] == channel_id[1] &&
               _data[14] == channel_id[2] && _data[15] == channel_id[3] && _data[16] == 0x57 && _data[17] == 0x42 &&
               _data[18] == channel_id[0] && _data[19] == channel_id[1] && _data[20] == channel_id[2] &&
               _data[21] == channel_id[3];
    }

  private:
    bool IsDataFrame() const { return _data.size() >= 2 && _data[0] == _dataHeader[0] && _data[1] == _dataHeader[1]; }

    bool HasValidAirGndId() const { return _data.size() >= 18 && _data[10] == _data[16]; }

    bool HasValidRadioPort() const { return _data.size() >= 22 && _data[15] == _data[21]; }
};

class WifiFrame {
  public:
    WifiFrame(const std::span<uint8_t> &rawData) {
        // Frame Control (2 bytes)
        frameControl = (rawData[1] << 8) | rawData[0];

        // Duration/ID (2 bytes)
        durationID = (rawData[3] << 8) | rawData[2];

        // Receiver Address (6 bytes)
        receiverAddress.assign(rawData.begin() + 4, rawData.begin() + 10);

        // Transmitter Address (6 bytes)
        transmitterAddress.assign(rawData.begin() + 10, rawData.begin() + 16);

        // Destination Address (6 bytes)
        destinationAddress.assign(rawData.begin() + 16, rawData.begin() + 22);

        // Source Address (6 bytes)
        // sourceAddress.assign(rawData.begin() + 22, rawData.begin() + 28);

        // Sequence Control (2 bytes)
        sequenceControl = (rawData[22] << 8) | rawData[22];

        // Frame Body (variable length)
        // For simplicity, let's assume the body starts at byte 30
        //        frameBody.assign(rawData.begin() + 30, rawData.end() - 4);
        //
        //        // Frame Check Sequence (4 bytes, assuming little-endian)
        //        frameCheckSequence = (rawData[rawData.size() - 1] << 24) |
        //                                   (rawData[rawData.size() - 2] << 16) |
        //                                   (rawData[rawData.size() - 3] << 8) |
        //                                   rawData[rawData.size() - 4];
    }

    uint16_t frameControl;
    uint16_t durationID;
    std::vector<uint8_t> receiverAddress;
    std::vector<uint8_t> transmitterAddress;
    std::vector<uint8_t> destinationAddress;
    std::vector<uint8_t> sourceAddress;
    uint16_t sequenceControl;
    std::vector<uint8_t> frameBody;
    uint32_t frameCheckSequence;
};

#endif // LIBUSBDEMO_RXFRAME_H
