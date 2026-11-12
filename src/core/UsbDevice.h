// OpenBridge - direct libusb access to an Elektron device in Overbridge mode.
//
// USB layout (Overbridge USB driver, OverbridgeUSBDriver.sys 2.26.10, descriptor parser):
//   configuration 1
//   interface 1 alt 3 : audio device->host, interrupt IN  0x83
//   interface 2 alt 3 : audio host->device, interrupt OUT 0x03
//   interface 3       : Overbridge messages, one IN + one OUT bulk endpoint
//   interfaces 4/5    : class-compliant USB MIDI (left to the kernel / ALSA)
// Vendor control requests to the device (bmRequestType 0xC0 IN / 0x40 OUT):
//   req 1 IN  32 B : device name           req 1 OUT 32 B : set device name
//   req 4 IN   4 B : two big-endian u16 (protocol versions)
//   req 5 IN   4 B, req 6 IN : further properties
#pragma once

#include "DeviceDesc.h"
#include "RingBuffer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace ob {

struct UsbDeviceInfo {
    const DeviceDesc* desc = nullptr;
    uint8_t bus = 0;
    uint8_t address = 0;
    std::string serial;
    std::string id() const; // stable-ish key: "<pid>:<serial>" or "<pid>@bus,addr"
};

struct ProtocolVersions {
    uint16_t a = 0, b = 0;
};

struct StreamStats {
    std::atomic<uint64_t> inTransfers{0}, outTransfers{0};
    std::atomic<uint64_t> inErrors{0}, outErrors{0};
    std::atomic<uint64_t> toHostOverflows{0}, toDeviceUnderruns{0};
    std::atomic<float> toHostPeak[64];
    std::atomic<float> toDevicePeak[16];
};

class UsbDevice {
public:
    UsbDevice();
    ~UsbDevice();
    UsbDevice(const UsbDevice&) = delete;
    UsbDevice& operator=(const UsbDevice&) = delete;

    static std::vector<UsbDeviceInfo> enumerate(std::string* error = nullptr);

    bool open(const UsbDeviceInfo& info, std::string& error);
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    const UsbDeviceInfo& info() const { return info_; }
    const DeviceDesc& desc() const { return *info_.desc; }

    // Vendor control requests.
    bool readName(std::string& name);
    bool writeName(const std::string& name);
    bool readProtocolVersions(ProtocolVersions& v);
    bool vendorIn(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len, int& got);

    // Audio streaming. blocksPerTransfer: 7 frames each (default 24 -> 168 frames, 3.5 ms).
    bool startAudio(int blocksPerTransfer, std::string& error);
    void stopAudio();
    bool audioRunning() const { return running_.load(); }
    bool failed() const { return failed_.load(); }
    std::string lastError() const;

    int framesPerTransfer() const { return blocksPerTransfer_ * kFramesPerBlock; }

    // Device -> host frames (interleaved, desc().toHost.size() channels). Consumer: plugin.
    FrameRing& toHost() { return toHost_; }
    // Host -> device frames (interleaved, desc().toDevice.size() channels). Producer: plugin.
    FrameRing& toDevice() { return toDevice_; }
    StreamStats& stats() { return stats_; }

    // Raw access for the message interface (interface 3), used by the protocol layer.
    libusb_device_handle* handle() { return handle_; }

private:
    static void inCallback(libusb_transfer* t);
    static void outCallback(libusb_transfer* t);
    void submitIn();
    void submitOut();
    void decodeIn(const uint8_t* data, int len);
    void encodeOut(uint8_t* data);
    void eventLoop();
    void setError(const std::string& e);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    UsbDeviceInfo info_;

    int blocksPerTransfer_ = 24;
    int inBlockLen_ = 0, outBlockLen_ = 0;
    std::vector<uint8_t> inBuf_, outBuf_;
    std::vector<float> inFrames_, outFrames_;
    libusb_transfer* inXfr_ = nullptr;
    libusb_transfer* outXfr_ = nullptr;
    uint16_t outFrameCounter_ = 0;
    std::atomic<int> pending_{0};

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> failed_{false};
    mutable std::mutex errorLock_;
    std::string lastError_;

    FrameRing toHost_, toDevice_;
    StreamStats stats_;
};

} // namespace ob
