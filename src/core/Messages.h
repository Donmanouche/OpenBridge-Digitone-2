// Overbridge message channel (USB interface 3: bulk OUT 0x02, interrupt IN 0x82).
// Header layout:
//
//   0  u8      channel id (replies carry the same id)
//   1  u8      header version (2)
//   2  u32 BE  transaction sequence number
//   6  u32 BE  0xFFFFFFFF
//   10 u32 BE  offset of this chunk in the transaction
//   14 u8      message class   (0x10 edit, 0x32/0x33 realtime, 0x43..0x45 storage ...)
//   15 u8      message type    (object type for edits: 5 sound, 9 fx, 0x1b settings ...)
//   16 u16 BE  argument (track / index)
//   18 u32 BE  total payload length (bit 31: payload is compressed)
//   22 u16 BE  payload length in this chunk
//   24 u16 BE  16-bit byte sum of the payload (generic transactions; 0 for automation)
//   26 u16 BE  16-bit byte sum of the payload (automation messages; 0 otherwise)
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob {

constexpr int kMsgHeaderBytes = 28;
constexpr uint8_t kMsgVersion = 2;
constexpr uint8_t kAutomationChannel = 0xE0;
constexpr unsigned char kMsgOutEp = 0x02;
constexpr unsigned char kMsgInEp = 0x82;
constexpr int kMsgInterface = 3;

struct MsgHeader {
    uint8_t channel = 0;
    uint8_t version = kMsgVersion;
    uint32_t seq = 0;
    uint32_t reserved = 0xFFFFFFFF;
    uint32_t offset = 0;
    uint8_t cls = 0;
    uint8_t type = 0;
    uint16_t arg = 0;
    uint32_t totalLength = 0; // without the compression flag
    bool compressed = false;
    uint16_t chunkLength = 0;
    uint16_t sumA = 0; // offset 24
    uint16_t sumB = 0; // offset 26

    void write(uint8_t* out) const;
    static bool read(const uint8_t* in, MsgHeader& h);
};

uint16_t byteSum16(const uint8_t* data, size_t n);

// Object types used with class 0x10 edits / automation.
enum class EditObject : uint8_t { Sound = 5, Fx = 9, Settings = 0x1b };

// Single parameter change.
// For a sound: arg = track, payload = {track, deviceParamIndex, value} as u16 BE.
std::vector<uint8_t> makeAutomation(uint32_t seq, uint8_t objectType, uint16_t arg, uint16_t a, uint16_t b, uint16_t value);

// Generic transaction (single chunk; payload up to 995 bytes).
std::vector<uint8_t> makeTransaction(uint8_t channel, uint32_t seq, uint8_t cls, uint8_t type, uint16_t arg,
                                     const uint8_t* payload, uint16_t len);

} // namespace ob
