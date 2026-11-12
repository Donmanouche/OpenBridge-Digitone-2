#include "Messages.h"

#include <algorithm>

namespace ob {

namespace {
void be16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
void be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
} // namespace

uint16_t byteSum16(const uint8_t* data, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; ++i) s += data[i];
    return (uint16_t)s;
}

void MsgHeader::write(uint8_t* o) const
{
    o[0] = channel;
    o[1] = version;
    be32(o + 2, seq);
    be32(o + 6, reserved);
    be32(o + 10, offset);
    o[14] = cls;
    o[15] = type;
    be16(o + 16, arg);
    be32(o + 18, (totalLength & 0x7fffffffu) | (compressed ? 0x80000000u : 0));
    be16(o + 22, chunkLength);
    be16(o + 24, sumA);
    be16(o + 26, sumB);
}

bool MsgHeader::read(const uint8_t* in, MsgHeader& h)
{
    h.channel = in[0];
    h.version = in[1];
    h.seq = rd32(in + 2);
    h.reserved = rd32(in + 6);
    h.offset = rd32(in + 10);
    h.cls = in[14];
    h.type = in[15];
    h.arg = rd16(in + 16);
    const uint32_t t = rd32(in + 18);
    h.totalLength = t & 0x7fffffffu;
    h.compressed = (t & 0x80000000u) != 0;
    h.chunkLength = rd16(in + 22);
    h.sumA = rd16(in + 24);
    h.sumB = rd16(in + 26);
    return h.version == kMsgVersion;
}

std::vector<uint8_t> makeAutomation(uint32_t seq, uint8_t objectType, uint16_t arg, uint16_t a, uint16_t b, uint16_t value)
{
    std::vector<uint8_t> m(kMsgHeaderBytes + 6);
    uint8_t* p = m.data() + kMsgHeaderBytes;
    be16(p, a);
    be16(p + 2, b);
    be16(p + 4, value);
    MsgHeader h;
    h.channel = kAutomationChannel;
    h.seq = seq;
    h.cls = 0x10;
    h.type = objectType;
    h.arg = arg;
    h.totalLength = 6;
    h.chunkLength = 6;
    h.sumA = 0;
    h.sumB = byteSum16(p, 6);
    h.write(m.data());
    return m;
}

std::vector<uint8_t> makeTransaction(uint8_t channel, uint32_t seq, uint8_t cls, uint8_t type, uint16_t arg,
                                     const uint8_t* payload, uint16_t len)
{
    std::vector<uint8_t> m(kMsgHeaderBytes + len);
    if (len) std::copy(payload, payload + len, m.begin() + kMsgHeaderBytes);
    MsgHeader h;
    h.channel = channel;
    h.seq = seq;
    h.cls = cls;
    h.type = type;
    h.arg = arg;
    h.totalLength = len;
    h.chunkLength = len;
    h.sumA = byteSum16(payload, len);
    h.write(m.data());
    return m;
}

} // namespace ob
