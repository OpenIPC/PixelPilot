#include "BufferedPacketQueue.h"  // the class under test
#include <gtest/gtest.h>
#include <cstdint>
#include <iostream>
#include <vector>

// ---------- Test fixture ----------------------------------------------------
class BufferedPacketQueueTest : public ::testing::Test
{
  protected:
    BufferedPacketQueue   q;
    std::vector<uint16_t> delivered;

    // The queue bounds how long it will hold a packet back, so the tests drive the clock
    // themselves instead of letting a loaded machine decide whether the bound was hit.
    QueueTimePoint now{};

    void SetUp() override
    {
        delivered.clear();
        now = QueueTimePoint{};
    }

    void advance(int ms) { now += std::chrono::milliseconds(ms); }

    /* Helper: feed one packet and record what the queue actually delivers. */
    void feed(uint16_t seq)
    {
        uint16_t dummy = seq;

        auto cb = [this](const uint8_t* seq, std::size_t)
        {
            std::cout << "Delivered packet with sequence: " << *(uint16_t*) seq << std::endl;
            delivered.push_back(*(uint16_t*) seq);
        };

        q.processPacket(seq, (uint8_t*) &dummy, 2, cb, now);
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

// ---------- A gap that will never be filled --------------------------------
// The common case on a lossy link: FEC could not recover one packet, and every packet after it
// is held back waiting for it. Nothing is lost by waiting, but everything behind the gap gets
// later and later, so the queue has to give up at some point.
TEST_F(BufferedPacketQueueTest, PermanentGapDoesNotHoldTheStreamForFifteenPackets)
{
    for (uint16_t s = 1; s <= 5; ++s) feed(s);
    ASSERT_EQ(delivered, (std::vector<uint16_t>{1, 2, 3, 4, 5}));

    // 6 never arrives.
    for (uint16_t s = 7; s <= 11; ++s) feed(s);
    feed(12);

    ASSERT_EQ(delivered, (std::vector<uint16_t>{1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12}))
        << "A monotonic run past the gap should release the buffer, not wait for MAX_BUFFER_SIZE";
}

// The monotonic run is counted in packets, so how much latency it costs depends on the packet
// rate - which on the audio stream is a fraction of the video one. The age bound is what makes
// the wait the same on both.
TEST_F(BufferedPacketQueueTest, StaleBufferIsFlushedOnTimeout)
{
    feed(1);
    // 2 never arrives.
    feed(3);
    ASSERT_EQ(delivered, (std::vector<uint16_t>{1})) << "3 is held back waiting for 2";

    advance(25);
    feed(4);

    ASSERT_EQ(delivered, (std::vector<uint16_t>{1, 3, 4}))
        << "Once the buffer is older than MAX_BUFFER_AGE it has to be released";
}

// The other side of that bound: a reorder that resolves quickly must still be reordered, not
// flushed out of sequence.
TEST_F(BufferedPacketQueueTest, ReorderWithinTimeoutIsStillPutBackInOrder)
{
    feed(1);
    feed(3);
    advance(5);
    feed(2);

    ASSERT_EQ(delivered, (std::vector<uint16_t>{1, 2, 3}))
        << "A packet that arrives late but within MAX_BUFFER_AGE must not be flushed early";
}

// A large jump is not a reorder - it is a stream that restarted somewhere else. It must not be
// mistaken for a monotonic run, and the buffer cap has to catch it.
TEST_F(BufferedPacketQueueTest, LargeJumpFallsBackToTheBufferCap)
{
    feed(1);
    for (uint16_t s = 30000; s < 30000 + MAX_BUFFER_SIZE; ++s) feed(s);

    ASSERT_EQ(delivered.size(), 1u + MAX_BUFFER_SIZE) << "The buffer cap should have released the jumped-to block";
    ASSERT_EQ(delivered.front(), 1);
    ASSERT_EQ(delivered.back(), static_cast<uint16_t>(30000 + MAX_BUFFER_SIZE - 1));
}

// A flush delivers everything it is holding, so the pointer has to end up past all of it.
// Rewinding it to whichever packet happened to trigger the flush re-creates the stall that
// was just cleared.
TEST_F(BufferedPacketQueueTest, FlushAdvancesPastEverythingItDelivered)
{
    for (uint16_t s = 1; s <= 5; ++s) feed(s);

    // 6 never arrives, and 11 is missing from the run so the flush is triggered by 10 while
    // 12 is already buffered behind it.
    feed(7);
    feed(8);
    feed(9);
    feed(12);
    feed(10);
    ASSERT_EQ(delivered, (std::vector<uint16_t>{1, 2, 3, 4, 5, 7, 8, 9, 10, 12}));

    feed(13);
    ASSERT_EQ(delivered, (std::vector<uint16_t>{1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13}))
        << "13 follows the highest packet already delivered and must not be held back";
}

// RTP starts at a random sequence number, so a VTX rebooting mid-session shows up as a jump
// that can be more than half the sequence space - where a signed 16 bit distance reads as
// "behind us" and the queue would never move again.
TEST_F(BufferedPacketQueueTest, StreamRestartFarAheadResyncs)
{
    feed(1);
    feed(2);

    feed(40000);
    ASSERT_EQ(delivered, (std::vector<uint16_t>{1, 2})) << "40000 is held back at first";

    advance(25);
    feed(40001);
    feed(40002);

    ASSERT_EQ(delivered, (std::vector<uint16_t>{1, 2, 40000, 40001, 40002}))
        << "after the age bound releases 40000 the queue has to follow the new base";
}

// The buffer is a hash map, so the flush has to sort it - and sorting by raw value puts a
// block that straddles the wrap point in the wrong order.
TEST_F(BufferedPacketQueueTest, FlushAcrossTheWrapDeliversInOrder)
{
    feed(65532);

    // 65533 never arrives; the rest of the run crosses zero.
    feed(65534);
    feed(65535);
    feed(0);
    feed(1);
    feed(2);
    feed(3);

    ASSERT_EQ(delivered, (std::vector<uint16_t>{65532, 65534, 65535, 0, 1, 2, 3}))
        << "sorted by value this comes out as 0, 1, 2, 65534, 65535";
}

// ---------- gtest boilerplate main -----------------------------------------
int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
