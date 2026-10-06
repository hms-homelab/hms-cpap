#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::oxyii {

/**
 * The O2Ring-S (T8520) "OxyII" protocol: frames, payloads and replies.
 *
 * Pure functions with no Bluetooth dependency, so the test binary covers every
 * byte the direct BLE client sends. The original O2Ring's 0xAA protocol is a
 * different codec and stays in O2RingBleClient.
 *
 * Frame, both directions:
 *   A5 | cmd | ~cmd | flag | seq | len_lo | len_hi | payload | crc
 * flag is 0 on a request and 1 on a reply; crc is CRC-8 poly 0x07, init 0,
 * over every byte before it.
 */

constexpr uint8_t kLead = 0xA5;
constexpr size_t kHeaderLen = 7;
constexpr size_t kOverhead = kHeaderLen + 1;
// A file chunk is at most 512 bytes; a header claiming more is noise.
constexpr size_t kMaxPayload = 1024;

constexpr uint8_t kOpGetConfig = 0x00;
constexpr uint8_t kOpLive = 0x04;        // 24-byte header with SpO2, HR, motion, battery
constexpr uint8_t kOpSetup = 0x10;       // payload 00, required after auth
constexpr uint8_t kOpSetTime = 0xC0;
constexpr uint8_t kOpGetInfo = 0xE1;
constexpr uint8_t kOpGetBattery = 0xE4;  // reply byte [1] is the percentage
constexpr uint8_t kOpFileList = 0xF1;
constexpr uint8_t kOpFileStart = 0xF2;
constexpr uint8_t kOpFileData = 0xF3;
constexpr uint8_t kOpFileEnd = 0xF4;
constexpr uint8_t kOpAuth = 0xFF;        // one-way, never answered

constexpr size_t kNameLen = 14;          // YYYYMMDDhhmmss
// The ring's names carry no extension; this one tells every reader the format.
constexpr const char* kFileExt = ".o2s";

constexpr const char* kServiceUuid = "e8fb0001-a14b-98f9-831b-4e2941d01248";
constexpr const char* kWriteUuid = "e8fb0002-a14b-98f9-831b-4e2941d01248";
constexpr const char* kNotifyUuid = "e8fb0003-a14b-98f9-831b-4e2941d01248";
// Not matched on: the O2Ring-S advertises manufacturer ID 0xF34E, but so does
// the original O2Ring.

uint8_t crc8(const uint8_t* data, size_t len);

std::vector<uint8_t> encode(uint8_t op, uint8_t seq, const std::vector<uint8_t>& payload = {});

struct Frame {
    uint8_t op = 0;
    std::vector<uint8_t> payload;
};

/// Takes the first whole, valid frame out of buf, dropping any bytes in front
/// of it that cannot start one. Leaves a partial frame in place for the next
/// notification.
std::optional<Frame> takeFrame(std::vector<uint8_t>& buf);

/// cmd 0xFF: the session key XOR MD5("lepucloud"), serial prefix "0000", ts in
/// bytes 12..15 as (ts >> 0..3), as the ring expects.
std::vector<uint8_t> authPayload(uint32_t ts);

struct DateTime {
    uint16_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

/// cmd 0xC0: year LE, month, day, hour, minute, second, 0.
std::vector<uint8_t> timePayload(const DateTime& t);

/// Seconds since 1970 for t taken as UTC.
uint32_t epochOf(const DateTime& t);

struct Info {
    std::string firmware;
    std::string serial;
    DateTime clock;          // the ring's own clock, as it was set
    bool clock_valid = false;
};

/// cmd 0xE1 reply (60 bytes on the firmware documented).
std::optional<Info> parseInfo(const std::vector<uint8_t>& p);

struct Live {
    uint8_t state = 0;       // 0 no finger, 1 idle, 3 a file handle is open
    uint8_t spo2 = 0, hr = 0, motion = 0, battery = 0;
    bool reading = false;    // a finger and a plausible SpO2 and HR
};

/// cmd 0x04 reply: [5] state, [6] SpO2, [7] motion, [8] HR, [13] battery.
std::optional<Live> parseLive(const std::vector<uint8_t>& p);

/// cmd 0xF1 reply: u8 count, then 16-byte slots of a 14-digit name. Every
/// valid name, as "<name>.o2s", in the ring's order.
std::vector<std::string> parseFileList(const std::vector<uint8_t>& p);

/// cmd 0xF2 payload from "<name>" or "<name>.o2s"; nullopt if not a ring name.
std::optional<std::vector<uint8_t>> fileStartPayload(const std::string& filename);

/// cmd 0xF3 payload: the u32 LE offset.
std::vector<uint8_t> fileDataPayload(uint32_t offset);

uint32_t le32(const uint8_t* p);

/// An advertised name that is an O2Ring-S: "S8-AW ..." idle, "T8520_xxxx" worn.
bool nameMatches(const std::string& name);

} // namespace hms_cpap::oxyii
