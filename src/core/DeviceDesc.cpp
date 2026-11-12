#include "DeviceDesc.h"

namespace ob {

int DeviceDesc::toHostFrameBytes() const
{
    int n = 0;
    for (auto& c : toHost) n += c.bytes;
    return n;
}

int DeviceDesc::toDeviceFrameBytes() const
{
    int n = 0;
    for (auto& c : toDevice) n += c.bytes;
    return n;
}

namespace {

void addStereo(std::vector<Channel>& v, const std::string& base, int bytes)
{
    v.push_back({base + " L", bytes});
    v.push_back({base + " R", bytes});
}

// Digitakt II and Digitone II share one layout (manual D.8; byte sizes as observed by
// Overwitch: the first 14 device->host channels use 4-byte slots, the others 3 bytes).
DeviceDesc makeGen2(uint16_t pid, const char* name, const char* shortName)
{
    DeviceDesc d{pid, name, shortName, StreamFormat::V2_1, {}, {}};
    addStereo(d.toHost, "Main", 4);
    for (int t = 1; t <= 16; ++t)
        addStereo(d.toHost, "Track " + std::to_string(t), t <= 6 ? 4 : 3);
    addStereo(d.toHost, "Delay", 3);
    addStereo(d.toHost, "Reverb", 3);
    addStereo(d.toHost, "Chorus", 3);
    addStereo(d.toHost, "External In", 3);

    addStereo(d.toDevice, "Main", 4);
    addStereo(d.toDevice, "Delay", 4);
    addStereo(d.toDevice, "Reverb", 4);
    addStereo(d.toDevice, "Chorus", 4);
    return d;
}

DeviceDesc makeDigitone()
{
    DeviceDesc d{0x0b36, "Digitone", "DN", StreamFormat::V2, {}, {}};
    addStereo(d.toHost, "Main", 4);
    for (int t = 1; t <= 4; ++t) addStereo(d.toHost, "Track " + std::to_string(t), 4);
    addStereo(d.toHost, "Input", 4);
    addStereo(d.toDevice, "Main", 4);
    return d;
}

std::vector<DeviceDesc> makeAll()
{
    std::vector<DeviceDesc> v;
    v.push_back(makeGen2(0x0b34, "Digitone II", "DNII"));
    v.push_back(makeGen2(0x0b2b, "Digitakt II", "DTII"));
    v.push_back(makeDigitone());
    return v;
}

} // namespace

const std::vector<DeviceDesc>& allDeviceDescs()
{
    static const std::vector<DeviceDesc> descs = makeAll();
    return descs;
}

const DeviceDesc* findDeviceDesc(uint16_t vid, uint16_t pid)
{
    if (vid != kElektronVid) return nullptr;
    for (auto& d : allDeviceDescs())
        if (d.pid == pid) return &d;
    return nullptr;
}

std::vector<StereoBus> groupChannels(const std::vector<Channel>& channels)
{
    std::vector<StereoBus> buses;
    for (size_t i = 0; i < channels.size();) {
        const auto& n = channels[i].name;
        if (i + 1 < channels.size() && n.size() > 2 && n.compare(n.size() - 2, 2, " L") == 0) {
            const auto base = n.substr(0, n.size() - 2);
            if (channels[i + 1].name == base + " R") {
                buses.push_back({base, (int)i, 2});
                i += 2;
                continue;
            }
        }
        buses.push_back({n, (int)i, 1});
        ++i;
    }
    return buses;
}

} // namespace ob
