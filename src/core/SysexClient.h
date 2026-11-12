// Elektron SysEx file-system protocol over the device's USB MIDI port (ALSA sequencer).
// Works while the device is in Overbridge mode. Used for the +Drive preset library:
// listing /soundbanks/<A..H> and reading preset files (name, tags).
//
// Framing: F0 00 20 3C 10 00 <7-bit packed [seq u16, 0 u16, cmd, payload...]> F7.
// The reply carries cmd | 0x80 and our sequence number in bytes 2..3.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct _snd_seq;

namespace ob {

struct FsEntry {
    std::string name;
    bool directory = false;
    int32_t id = 0;        // slot number for presets (1-based)
    uint32_t size = 0;
    uint16_t operations = 0; // permitted operations; without the write bit (0x08) the entry is write-protected
    bool valid = false;
    bool hasMetadata = false;
    bool writeProtected() const { return (operations & 0x08) == 0; }
};

class SysexClient {
public:
    SysexClient() = default;
    ~SysexClient();
    SysexClient(const SysexClient&) = delete;
    SysexClient& operator=(const SysexClient&) = delete;

    // Finds the ALSA sequencer port whose client name contains `deviceName` and connects to it.
    bool open(const std::string& deviceName, std::string& error);
    void close();
    bool isOpen() const { return seq_ != nullptr; }

    // Low level: sends cmd + payload and waits for the reply (decoded, starting at the cmd byte).
    bool request(uint8_t cmd, const std::vector<uint8_t>& payload, std::vector<uint8_t>& reply, int timeoutMs = 1000);

    bool listDir(const std::string& path, std::vector<FsEntry>& out);
    bool readFile(const std::string& path, std::vector<uint8_t>& out);

    static std::vector<uint8_t> encode7(const uint8_t* data, size_t n);
    static std::vector<uint8_t> decode7(const uint8_t* data, size_t n);

private:
    bool send(const std::vector<uint8_t>& msg);
    bool receive(std::vector<uint8_t>& msg, int timeoutMs);

    _snd_seq* seq_ = nullptr;
    int port_ = -1;
    uint16_t nextSeq_ = 1;
    std::vector<uint8_t> rx_;
};

// Preset files on the +Drive: 36-byte file header, then the sound container (BEEFBACE...).
struct PresetInfo {
    std::string name;
    uint32_t tags = 0;
};
bool parsePresetFile(const std::vector<uint8_t>& file, PresetInfo& out);

// The 32 preset tags, in bit order (bit 0 = Kick). Same order as the device's label grid.
const char* presetTagName(int bit);
std::string presetTagList(uint32_t tags);

} // namespace ob
