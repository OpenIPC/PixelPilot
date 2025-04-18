#include "BufferedPacketQueue.h"  // the class under test
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

// ---------- Test fixture ----------------------------------------------------
class BufferedPacketQueueTest : public ::testing::Test
{
  protected:
    BufferedPacketQueue   q;
    std::vector<uint16_t> delivered;

    void SetUp() override { delivered.clear(); }

    /* Helper: feed one packet and record what the queue actually delivers. */
    void feed(uint16_t seq)
    {
        uint16_t dummy = seq;

        auto cb = [this](const uint8_t* seq, std::size_t)
        {
            std::cout << "Delivered packet with sequence: " << *(uint16_t*) seq << std::endl;
            delivered.push_back(*(uint16_t*) seq);
        };

        q.processPacket(seq, (uint8_t*) &dummy, 2, cb);
    }
};

// ---------- The reproduction test ------------------------------------------
TEST_F(BufferedPacketQueueTest, WrapAroundDelivers)
{
    feed(65534);
    feed(65535);
    ASSERT_EQ(delivered, (std::vector<uint16_t>{65534, 65535}));

    for (uint16_t s = 0; s < 25; ++s) feed(s);

    std::vector<uint16_t> expected = {65534, 65535};
    for (uint16_t s = 0; s < 25; ++s) expected.push_back(s);

    ASSERT_EQ(delivered, expected) << "Overflow flush should deliver the entire block in one shot";
}

TEST_F(BufferedPacketQueueTest, ReorderedDeliversInOrder)
{
    feed(65533);
    feed(65535);
    feed(65534);
    ASSERT_EQ(delivered, (std::vector<uint16_t>{65533, 65534, 65535}));

    for (uint16_t s = 0; s < 25; ++s) feed(s);

    std::vector<uint16_t> expected = {65533, 65534, 65535};
    for (uint16_t s = 0; s < 25; ++s) expected.push_back(s);

    ASSERT_EQ(delivered, expected) << "Overflow flush should deliver the entire block in one shot";
}

// ---------- gtest boilerplate main -----------------------------------------
int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
