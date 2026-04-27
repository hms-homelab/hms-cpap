#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <cstring>

namespace hms_cpap {
namespace fysetc {

enum class MsgType : uint8_t {
    HELLO            = 0x01,
    HELLO_ACK        = 0x02,
    SECTOR_READ_REQ  = 0x10,
    SECTOR_READ_RESP = 0x11,
    SECTOR_READ_ERR  = 0x12,
    LOG              = 0x20,
    STATUS           = 0x30,
    PING             = 0x40,
    PONG             = 0x41,
    BYE              = 0x50,
};

enum class SectorReadStatus : uint8_t {
    OK       = 0x00,
    PARTIAL  = 0x01,
    ERR      = 0x02,
};

enum class SectorReadErrCode : uint8_t {
    CARD_NOT_READY = 0x01,
    READ_FAILED    = 0x02,
    BUS_BUSY       = 0x03,
    TIMEOUT        = 0x04,
};

enum class LogLevel : uint8_t {
    NONE    = 0,
    ERR     = 1,
    WARN    = 2,
    INFO    = 3,
    DEBUG   = 4,
    VERBOSE = 5,
};

enum class ByeReason : uint8_t {
    NORMAL        = 0x00,
    REBOOT        = 0x01,
    CONFIG_CHANGE = 0x02,
    ERR           = 0x03,
};

// Wire header: [uint32 length][uint8 type][uint8 flags][uint16 req_id]
struct MsgHeader {
    uint32_t length = 0;
    MsgType  type   = MsgType::PING;
    uint8_t  flags  = 0;
    uint16_t req_id = 0;

    static constexpr size_t WIRE_SIZE = 8;
};

struct SectorRange {
    uint32_t sector_lba = 0;
    uint16_t count      = 0;
};

// --- Payload structs ---

struct HelloPayload {
    char     device_serial[16] = {};
    uint16_t fw_version        = 0;
    uint32_t boot_count        = 0;
    uint32_t uptime_ms         = 0;
    uint32_t free_heap         = 0;

    static constexpr size_t WIRE_SIZE = 30;
};

struct HelloAckPayload {
    uint64_t server_time_unix = 0;
    uint32_t session_id       = 0;

    static constexpr size_t WIRE_SIZE = 12;
};

struct StatusPayload {
    uint32_t uptime_ms       = 0;
    uint32_t free_heap       = 0;
    uint32_t yields          = 0;
    uint32_t steals          = 0;
    uint32_t reads_ok        = 0;
    uint32_t reads_err       = 0;
    uint32_t bus_hold_max_ms = 0;

    static constexpr size_t WIRE_SIZE = 28;
};

// --- Codec functions ---

inline std::vector<uint8_t> encodeHeader(MsgType type, uint8_t flags, uint16_t req_id,
                                          uint32_t payload_size) {
    uint32_t length = 4 + payload_size;  // type(1) + flags(1) + req_id(2) + payload
    std::vector<uint8_t> buf(MsgHeader::WIRE_SIZE);
    std::memcpy(&buf[0], &length, 4);
    buf[4] = static_cast<uint8_t>(type);
    buf[5] = flags;
    std::memcpy(&buf[6], &req_id, 2);
    return buf;
}

inline bool decodeHeader(const uint8_t* data, size_t len, MsgHeader& hdr) {
    if (len < MsgHeader::WIRE_SIZE) return false;
    std::memcpy(&hdr.length, data, 4);
    hdr.type = static_cast<MsgType>(data[4]);
    hdr.flags = data[5];
    std::memcpy(&hdr.req_id, &data[6], 2);
    return true;
}

inline std::vector<uint8_t> encodeHelloAck(uint16_t req_id, uint64_t server_time,
                                            uint32_t session_id) {
    auto buf = encodeHeader(MsgType::HELLO_ACK, 0, req_id, HelloAckPayload::WIRE_SIZE);
    buf.resize(MsgHeader::WIRE_SIZE + HelloAckPayload::WIRE_SIZE);
    std::memcpy(&buf[8], &server_time, 8);
    std::memcpy(&buf[16], &session_id, 4);
    return buf;
}

inline bool decodeHello(const uint8_t* payload, size_t len, HelloPayload& out) {
    if (len < HelloPayload::WIRE_SIZE) return false;
    std::memcpy(out.device_serial, payload, 16);
    std::memcpy(&out.fw_version, payload + 16, 2);
    std::memcpy(&out.boot_count, payload + 18, 4);
    std::memcpy(&out.uptime_ms, payload + 22, 4);
    std::memcpy(&out.free_heap, payload + 26, 4);
    return true;
}

inline std::vector<uint8_t> encodeSectorReadReq(uint16_t req_id,
                                                 const std::vector<SectorRange>& ranges) {
    uint8_t count = static_cast<uint8_t>(ranges.size());
    uint32_t payload_size = 1 + count * 6;
    auto buf = encodeHeader(MsgType::SECTOR_READ_REQ, 0, req_id, payload_size);
    buf.resize(MsgHeader::WIRE_SIZE + payload_size);
    buf[8] = count;
    for (size_t i = 0; i < ranges.size(); ++i) {
        size_t off = 9 + i * 6;
        std::memcpy(&buf[off], &ranges[i].sector_lba, 4);
        std::memcpy(&buf[off + 4], &ranges[i].count, 2);
    }
    return buf;
}

inline bool decodeSectorReadReq(const uint8_t* payload, size_t len,
                                 std::vector<SectorRange>& ranges) {
    if (len < 1) return false;
    uint8_t count = payload[0];
    if (len < 1 + count * 6) return false;
    ranges.resize(count);
    for (uint8_t i = 0; i < count; ++i) {
        size_t off = 1 + i * 6;
        std::memcpy(&ranges[i].sector_lba, payload + off, 4);
        std::memcpy(&ranges[i].count, payload + off + 4, 2);
    }
    return true;
}

inline bool decodeSectorReadResp(const uint8_t* payload, size_t len,
                                  uint32_t& sector_lba, uint16_t& count,
                                  SectorReadStatus& status,
                                  const uint8_t*& data_ptr, size_t& data_len) {
    if (len < 7) return false;
    std::memcpy(&sector_lba, payload, 4);
    std::memcpy(&count, payload + 4, 2);
    status = static_cast<SectorReadStatus>(payload[6]);
    data_ptr = payload + 7;
    data_len = count * 512;
    if (len < 7 + data_len) return false;
    return true;
}

inline bool decodeStatus(const uint8_t* payload, size_t len, StatusPayload& out) {
    if (len < StatusPayload::WIRE_SIZE) return false;
    std::memcpy(&out.uptime_ms, payload, 4);
    std::memcpy(&out.free_heap, payload + 4, 4);
    std::memcpy(&out.yields, payload + 8, 4);
    std::memcpy(&out.steals, payload + 12, 4);
    std::memcpy(&out.reads_ok, payload + 16, 4);
    std::memcpy(&out.reads_err, payload + 20, 4);
    std::memcpy(&out.bus_hold_max_ms, payload + 24, 4);
    return true;
}

inline std::vector<uint8_t> encodePing(uint16_t req_id, uint32_t nonce) {
    auto buf = encodeHeader(MsgType::PING, 0, req_id, 4);
    buf.resize(MsgHeader::WIRE_SIZE + 4);
    std::memcpy(&buf[8], &nonce, 4);
    return buf;
}

inline std::vector<uint8_t> encodeBye(uint16_t req_id, ByeReason reason) {
    auto buf = encodeHeader(MsgType::BYE, 0, req_id, 1);
    buf.resize(MsgHeader::WIRE_SIZE + 1);
    buf[8] = static_cast<uint8_t>(reason);
    return buf;
}

}  // namespace fysetc
}  // namespace hms_cpap
