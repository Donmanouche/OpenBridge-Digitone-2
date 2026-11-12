#include "SysexClient.h"

#include <alsa/asoundlib.h>

#include <chrono>
#include <cstring>
#include <poll.h>

namespace ob {

namespace {
const uint8_t kHeader[] = {0xF0, 0x00, 0x20, 0x3C, 0x10, 0x00};

void putU32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}
uint32_t getU32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// cp1252 / latin-1 names from the device -> UTF-8 (the device uses Å, Ä, Ö...).
std::string latin1ToUtf8(const char* s, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n && s[i]; ++i) {
        const auto c = (uint8_t)s[i];
        if (c < 0x80) out += (char)c;
        else {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}
} // namespace

SysexClient::~SysexClient() { close(); }

std::vector<uint8_t> SysexClient::encode7(const uint8_t* data, size_t n)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i < n; i += 7) {
        const size_t g = std::min<size_t>(7, n - i);
        uint8_t msb = 0;
        for (size_t k = 0; k < g; ++k)
            if (data[i + k] & 0x80) msb |= (uint8_t)(1 << (6 - k));
        out.push_back(msb);
        for (size_t k = 0; k < g; ++k) out.push_back(data[i + k] & 0x7F);
    }
    return out;
}

std::vector<uint8_t> SysexClient::decode7(const uint8_t* data, size_t n)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i < n; i += 8) {
        const uint8_t msb = data[i];
        for (size_t k = 1; k < 8 && i + k < n; ++k)
            out.push_back((uint8_t)(data[i + k] | ((msb & (1 << (7 - k))) ? 0x80 : 0)));
    }
    return out;
}

bool SysexClient::open(const std::string& deviceName, std::string& error)
{
    close();
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        error = "ALSA sequencer not available";
        return false;
    }
    snd_seq_set_client_name(seq, "OpenBridge SysEx");
    const int me = snd_seq_create_simple_port(seq, "io",
                                              SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE |
                                                  SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE |
                                                  SND_SEQ_PORT_CAP_NO_EXPORT,
                                              SND_SEQ_PORT_TYPE_APPLICATION);
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    int dc = -1, dp = -1;
    while (dc < 0 && snd_seq_query_next_client(seq, ci) >= 0) {
        if (!std::strstr(snd_seq_client_info_get_name(ci), deviceName.c_str())) continue;
        snd_seq_port_info_set_client(pi, snd_seq_client_info_get_client(ci));
        snd_seq_port_info_set_port(pi, -1);
        if (snd_seq_query_next_port(seq, pi) >= 0) {
            dc = snd_seq_port_info_get_client(pi);
            dp = snd_seq_port_info_get_port(pi);
        }
    }
    if (me < 0 || dc < 0 || snd_seq_connect_to(seq, me, dc, dp) < 0 || snd_seq_connect_from(seq, me, dc, dp) < 0) {
        error = "MIDI port of " + deviceName + " not found";
        snd_seq_close(seq);
        return false;
    }
    seq_ = seq;
    port_ = me;
    rx_.clear();
    return true;
}

void SysexClient::close()
{
    if (seq_) snd_seq_close(seq_);
    seq_ = nullptr;
    port_ = -1;
}

bool SysexClient::send(const std::vector<uint8_t>& msg)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port_);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_sysex(&ev, (unsigned)msg.size(), (void*)msg.data());
    for (int tries = 0; tries < 100; ++tries) {
        const int r = snd_seq_event_output_direct(seq_, &ev);
        if (r >= 0) return true;
        if (r != -EAGAIN) return false;
        poll(nullptr, 0, 2);
    }
    return false;
}

bool SysexClient::receive(std::vector<uint8_t>& msg, int timeoutMs)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        snd_seq_event_t* in = nullptr;
        const int r = snd_seq_event_input(seq_, &in);
        if (r == -EAGAIN || r == -ENOSPC || !in) {
            if (r == -EAGAIN) {
                pollfd pfd[4];
                const int n = snd_seq_poll_descriptors(seq_, pfd, 4, POLLIN);
                poll(pfd, (nfds_t)n, 10);
            }
            continue;
        }
        if (r < 0) return false;
        if (in->type != SND_SEQ_EVENT_SYSEX) continue;
        const auto* d = (const uint8_t*)in->data.ext.ptr;
        const size_t len = in->data.ext.len;
        if (len && d[0] == 0xF0) rx_.clear();
        rx_.insert(rx_.end(), d, d + len);
        if (!rx_.empty() && rx_.back() == 0xF7) {
            msg.swap(rx_);
            rx_.clear();
            return true;
        }
    }
    return false;
}

bool SysexClient::request(uint8_t cmd, const std::vector<uint8_t>& payload, std::vector<uint8_t>& reply, int timeoutMs)
{
    if (!seq_) return false;
    const uint16_t seq = nextSeq_++;
    if (nextSeq_ == 0) nextSeq_ = 1;
    std::vector<uint8_t> logical = {(uint8_t)(seq >> 8), (uint8_t)seq, 0, 0, cmd};
    logical.insert(logical.end(), payload.begin(), payload.end());
    std::vector<uint8_t> msg(kHeader, kHeader + sizeof kHeader);
    const auto enc = encode7(logical.data(), logical.size());
    msg.insert(msg.end(), enc.begin(), enc.end());
    msg.push_back(0xF7);
    if (!send(msg)) return false;

    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<uint8_t> in;
    while (std::chrono::steady_clock::now() < end) {
        const int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (!receive(in, std::max(left, 1))) return false;
        if (in.size() < sizeof kHeader + 2 || std::memcmp(in.data(), kHeader, sizeof kHeader) != 0) continue;
        auto d = decode7(in.data() + sizeof kHeader, in.size() - sizeof kHeader - 1);
        // [seq u16][request seq u16][cmd|0x80]...
        if (d.size() < 5 || d[4] != (cmd | 0x80) || (uint16_t)(d[2] << 8 | d[3]) != seq) continue;
        reply.assign(d.begin() + 4, d.end());
        return true;
    }
    return false;
}

bool SysexClient::listDir(const std::string& path, std::vector<FsEntry>& out)
{
    out.clear();
    std::vector<uint8_t> p(path.begin(), path.end());
    p.push_back(0);
    putU32(p, 0); // start
    putU32(p, 0); // end (0 = all)
    p.push_back(1);
    std::vector<uint8_t> r;
    if (!request(0x53, p, r, 3000) || r.size() < 2 || r[1] != 1) return false;
    // r: cmd, status, ... entries from offset 14 (18 in the full logical message).
    size_t i = 14;
    while (i < r.size()) {
        const auto* z = (const uint8_t*)std::memchr(r.data() + i, 0, r.size() - i);
        if (!z) break;
        FsEntry e;
        e.name = latin1ToUtf8((const char*)r.data() + i, (size_t)(z - (r.data() + i)));
        i = (size_t)(z - r.data()) + 1;
        if (i + 2 > r.size()) break;
        const uint8_t type = r[i + 1];
        i += 2;
        if (type == 1) {
            e.directory = true;
            i += 4;
        } else {
            if (i + 12 > r.size()) break;
            e.id = (int32_t)getU32(&r[i]);
            e.size = getU32(&r[i + 4]);
            e.operations = (uint16_t)(r[i + 8] << 8 | r[i + 9]);
            e.valid = r[i + 10] != 0;
            e.hasMetadata = r[i + 11] != 0;
            i += 12;
        }
        out.push_back(std::move(e));
    }
    return true;
}

bool SysexClient::readFile(const std::string& path, std::vector<uint8_t>& out)
{
    out.clear();
    std::vector<uint8_t> p(path.begin(), path.end());
    p.push_back(0);
    putU32(p, 0x2000); // chunk size
    p.push_back(0);    // uncompressed
    std::vector<uint8_t> r;
    if (!request(0x54, p, r) || r.size() < 6 || r[1] != 1) return false;
    const uint32_t job = getU32(&r[2]);
    bool ok = false;
    // Part 0 returns the file's information (no data); the device wants it read first.
    for (uint32_t part = 0; part < 4096; ++part) {
        std::vector<uint8_t> q;
        putU32(q, job);
        putU32(q, part);
        if (!request(0x55, q, r)) break;
        if (r.size() >= 2 && r[1] == 0) { // "File transfer is complete"
            ok = true;
            break;
        }
        if (part == 0) continue;
        // cmd, status, job u32, part u32, u32 (1000), u8, crc u32, size u32, data
        if (r.size() < 23) break;
        const uint32_t n = getU32(&r[19]);
        if (23 + (size_t)n > r.size()) break;
        out.insert(out.end(), r.begin() + 23, r.begin() + 23 + n);
        if (n == 0) {
            ok = true;
            break;
        }
    }
    std::vector<uint8_t> q;
    putU32(q, job);
    request(0x56, q, r);
    return ok;
}

bool parsePresetFile(const std::vector<uint8_t>& f, PresetInfo& out)
{
    // 36-byte file header, then the sound container: BEEFBACE, version u32, tags u32 BE, name[16].
    static const uint8_t magic[] = {0xBE, 0xEF, 0xBA, 0xCE};
    if (f.size() < 36 + 28 || std::memcmp(f.data() + 36, magic, 4) != 0) return false;
    out.tags = getU32(f.data() + 36 + 8);
    out.name = latin1ToUtf8((const char*)f.data() + 36 + 12, 16);
    return true;
}

const char* presetTagName(int bit)
{
    static const char* n[32] = {"Kick",     "Snare",    "Rimshot",    "Clap",     "Tom",        "Percussion", "Hi-Hat",
                                "Cymbal",   "Cowbell",  "Synth",      "Bass",     "Lead",       "Pad",        "Texture",
                                "Chord",    "Sound Fx", "Electronic", "Metallic", "Acoustic",   "Atmosphere", "Noisy",
                                "Glitch",   "Hard",     "Soft",       "Dark",     "Bright",     "Vintage",    "Epic",
                                "Fail",     "Loop",     "Mine",       "Favourite"};
    return bit >= 0 && bit < 32 ? n[bit] : "";
}

std::string presetTagList(uint32_t tags)
{
    std::string s;
    for (int b = 0; b < 32; ++b)
        if (tags & (1u << b)) {
            if (!s.empty()) s += ", ";
            s += presetTagName(b);
        }
    return s;
}

} // namespace ob
