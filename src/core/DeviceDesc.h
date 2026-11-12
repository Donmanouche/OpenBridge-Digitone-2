// OpenBridge - Linux implementation of the Elektron Overbridge protocol.
// Device descriptions: USB ids and audio channel layouts.
//
// Sources: Overbridge 2.26.10 user manual (appendix D, channel tables), the Overbridge
// USB driver (interfaces/endpoints parsed from descriptors) and Overwitch (GPL-3.0+,
// David García Goñi) for the sample formats of the "v2" and "v2.1" streams.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ob {

constexpr uint16_t kElektronVid = 0x1935;
constexpr double kDeviceSampleRate = 48000.0;
constexpr int kFramesPerBlock = 7;       // audio frames in one USB block
constexpr int kBlockHeaderBytes = 32;    // u16 header, u16 frame counter, 28 bytes padding

// Stream format family.
//  V2   : every sample is a big-endian int32 (Digitakt, Digitone, Syntakt, A4/AR MKII...)
//  V2_1 : 4-byte slots carry a right-aligned 24-bit big-endian sample, 3-byte slots a packed
//         24-bit big-endian sample (Digitakt II, Digitone II)
enum class StreamFormat { V2, V2_1 };

struct Channel {
    std::string name;
    int bytes; // 3 or 4
};

struct DeviceDesc {
    uint16_t pid;
    std::string name;       // product name ("Digitone II")
    std::string shortName;  // used for logos/ids ("DNII")
    StreamFormat format;
    std::vector<Channel> toHost;   // device -> computer (plugin outputs)
    std::vector<Channel> toDevice; // computer -> device (plugin inputs)

    int toHostFrameBytes() const;
    int toDeviceFrameBytes() const;
};

// Returns nullptr when the product id is not an Overbridge device known to OpenBridge.
const DeviceDesc* findDeviceDesc(uint16_t vid, uint16_t pid);
const std::vector<DeviceDesc>& allDeviceDescs();

// Stereo buses, built from consecutive "X L"/"X R" channel pairs (for the plugin's bus layout).
struct StereoBus {
    std::string name;
    int firstChannel;
    int channels; // 1 or 2
};
std::vector<StereoBus> groupChannels(const std::vector<Channel>& channels);

} // namespace ob
