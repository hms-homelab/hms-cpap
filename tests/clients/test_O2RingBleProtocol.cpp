#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

// Test the Viatom protocol functions directly (no BLE hardware needed)
// These are the same functions used by O2RingBleClient

namespace {

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t chk = crc ^ data[i];
        crc = 0;
        if (chk & 0x01) crc  = 0x07;
        if (chk & 0x02) crc ^= 0x0e;
        if (chk & 0x04) crc ^= 0x1c;
        if (chk & 0x08) crc ^= 0x38;
        if (chk & 0x10) crc ^= 0x70;
        if (chk & 0x20) crc ^= 0xe0;
        if (chk & 0x40) crc ^= 0xc7;
        if (chk & 0x80) crc ^= 0x89;
    }
    return crc;
}

std::vector<uint8_t> buildCmd(uint8_t cmd, uint16_t block = 0,
                               const std::vector<uint8_t>& payload = {}) {
    std::vector<uint8_t> frame;
    frame.push_back(0xAA);
    frame.push_back(cmd);
    frame.push_back(cmd ^ 0xFF);
    frame.push_back(block & 0xFF);
    frame.push_back((block >> 8) & 0xFF);
    uint16_t plen = static_cast<uint16_t>(payload.size());
    frame.push_back(plen & 0xFF);
    frame.push_back((plen >> 8) & 0xFF);
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(crc8(frame.data(), frame.size()));
    return frame;
}

} // anonymous namespace

// ── CRC-8 Tests ────────────────────────────────────────────────────────

TEST(ViatomProtocol, CRC8EmptyIsZero) {
    EXPECT_EQ(crc8(nullptr, 0), 0);
}

TEST(ViatomProtocol, CRC8KnownVectors) {
    // READ_SENSORS command: aa 17 e8 00 00 00 00 → CRC should be 0x1b
    uint8_t read_sensors[] = {0xAA, 0x17, 0xE8, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(crc8(read_sensors, 7), 0x1B);

    // INFO command: aa 14 eb 00 00 00 00 → CRC should be 0xc6
    uint8_t info[] = {0xAA, 0x14, 0xEB, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(crc8(info, 7), 0xC6);

    // FILE_CLOSE command: aa 05 fa 00 00 00 00 → CRC should be 0x21
    uint8_t file_close[] = {0xAA, 0x05, 0xFA, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(crc8(file_close, 7), 0x21);
}

// ── Command Building Tests ─────────────────────────────────────────────

TEST(ViatomProtocol, BuildInfoCommand) {
    auto cmd = buildCmd(0x14);
    ASSERT_EQ(cmd.size(), 8u);
    EXPECT_EQ(cmd[0], 0xAA);  // header
    EXPECT_EQ(cmd[1], 0x14);  // cmd
    EXPECT_EQ(cmd[2], 0xEB);  // cmd ^ 0xFF
    EXPECT_EQ(cmd[3], 0x00);  // block lo
    EXPECT_EQ(cmd[4], 0x00);  // block hi
    EXPECT_EQ(cmd[5], 0x00);  // length lo
    EXPECT_EQ(cmd[6], 0x00);  // length hi
    EXPECT_EQ(cmd[7], 0xC6);  // CRC
}

TEST(ViatomProtocol, BuildReadSensorsCommand) {
    auto cmd = buildCmd(0x17);
    ASSERT_EQ(cmd.size(), 8u);
    EXPECT_EQ(cmd[0], 0xAA);
    EXPECT_EQ(cmd[1], 0x17);
    EXPECT_EQ(cmd[7], 0x1B);
}

TEST(ViatomProtocol, BuildFileOpenCommand) {
    std::string filename = "20260418125023";
    std::vector<uint8_t> payload(filename.begin(), filename.end());
    payload.push_back(0x00); // null terminator

    auto cmd = buildCmd(0x03, 0, payload);
    ASSERT_EQ(cmd.size(), 8u + payload.size());
    EXPECT_EQ(cmd[0], 0xAA);
    EXPECT_EQ(cmd[1], 0x03);  // FILE_OPEN
    EXPECT_EQ(cmd[2], 0xFC);  // 0x03 ^ 0xFF
    // Length = 15 (14 chars + null)
    EXPECT_EQ(cmd[5], 15);
    EXPECT_EQ(cmd[6], 0);
    // Payload starts at offset 7
    EXPECT_EQ(cmd[7], '2');
    // Null terminator
    EXPECT_EQ(cmd[7 + 14], 0x00);
}

TEST(ViatomProtocol, BuildFileReadWithBlock) {
    auto cmd = buildCmd(0x04, 5);
    ASSERT_EQ(cmd.size(), 8u);
    EXPECT_EQ(cmd[1], 0x04);  // FILE_READ
    EXPECT_EQ(cmd[3], 5);     // block lo
    EXPECT_EQ(cmd[4], 0);     // block hi
}

TEST(ViatomProtocol, BuildFileReadBlockHighByte) {
    auto cmd = buildCmd(0x04, 300);
    EXPECT_EQ(cmd[3], 300 & 0xFF);   // 0x2C
    EXPECT_EQ(cmd[4], (300 >> 8) & 0xFF); // 0x01
}

// ── Frame Reassembly Tests ─────────────────────────────────────────────

TEST(ViatomProtocol, FrameValidation) {
    // Build a valid response frame (0x55 header)
    std::vector<uint8_t> frame = {0x55, 0x17, 0xE8, 0x00, 0x00, 0x02, 0x00};
    frame.push_back(96); // payload byte 1: spo2
    frame.push_back(72); // payload byte 2: hr
    frame.push_back(crc8(frame.data(), frame.size()));

    // Verify CRC
    uint8_t want = frame.back();
    uint8_t got = crc8(frame.data(), frame.size() - 1);
    EXPECT_EQ(want, got);

    // Verify structure
    EXPECT_EQ(frame[0], 0x55); // response header
    EXPECT_EQ(frame[1], 0x17); // READ_SENSORS echo
    EXPECT_EQ((frame[1] ^ 0xFF), frame[2]); // XOR check
    uint16_t plen = frame[5] | (frame[6] << 8);
    EXPECT_EQ(plen, 2u); // 2-byte payload
}

TEST(ViatomProtocol, FrameReassemblyFromChunks) {
    // Simulate a response split across 20-byte BLE MTU chunks
    // Full frame: [0x55, cmd, xor, block_lo, block_hi, len_lo, len_hi, ...payload..., CRC]
    std::vector<uint8_t> full_frame = {0x55, 0x14, 0xEB, 0x00, 0x00};
    // Payload: 30 bytes of JSON-like data
    std::vector<uint8_t> payload(30, 'A');
    uint16_t plen = static_cast<uint16_t>(payload.size());
    full_frame.push_back(plen & 0xFF);
    full_frame.push_back((plen >> 8) & 0xFF);
    full_frame.insert(full_frame.end(), payload.begin(), payload.end());
    full_frame.push_back(crc8(full_frame.data(), full_frame.size()));

    // Split into 20-byte chunks
    std::vector<std::vector<uint8_t>> chunks;
    for (size_t i = 0; i < full_frame.size(); i += 20) {
        size_t end = std::min(i + 20, full_frame.size());
        chunks.emplace_back(full_frame.begin() + i, full_frame.begin() + end);
    }

    EXPECT_EQ(chunks.size(), 2u); // 38 bytes = 20 + 18

    // Reassemble
    std::vector<uint8_t> reassembled;
    for (auto& chunk : chunks) {
        reassembled.insert(reassembled.end(), chunk.begin(), chunk.end());
    }

    EXPECT_EQ(reassembled, full_frame);

    // Verify CRC after reassembly
    uint8_t want = reassembled.back();
    uint8_t got = crc8(reassembled.data(), reassembled.size() - 1);
    EXPECT_EQ(want, got);
}

TEST(ViatomProtocol, RejectsBadCRC) {
    auto cmd = buildCmd(0x14);
    cmd.back() ^= 0xFF; // corrupt CRC

    uint8_t want = cmd.back();
    uint8_t got = crc8(cmd.data(), cmd.size() - 1);
    EXPECT_NE(want, got);
}

TEST(ViatomProtocol, RejectsBadXOR) {
    auto cmd = buildCmd(0x14);
    cmd[2] = 0x00; // corrupt XOR byte

    EXPECT_NE((cmd[1] ^ 0xFF), cmd[2]);
}
