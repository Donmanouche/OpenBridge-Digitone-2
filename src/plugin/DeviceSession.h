// A connection to one Overbridge device, shared by the plugin instances of a process.
// Owns the UsbDevice, reconnects it after an unplug, and hands its audio rings to the
// audio thread through a small begin/end guard so they are never reconfigured while in use.
#pragma once

#include "core/Messenger.h"
#include "core/UsbDevice.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ob {

class DeviceSession {
public:
    enum class State { Searching, Connecting, Running, Error };

    // pid: product wanted (Digitone II = 0x0b34). wantedId: a UsbDeviceInfo::id() or "" (first found).
    static std::shared_ptr<DeviceSession> acquire(uint16_t pid, const std::string& wantedId);
    ~DeviceSession();

    State state() const { return state_.load(); }
    std::string statusText() const;
    std::string deviceName() const;
    std::string deviceId() const;
    const DeviceDesc* desc() const { return desc_; }
    std::vector<UsbDeviceInfo> availableDevices() const;

    // Audio thread: returns the device when its rings may be used until endAudio().
    UsbDevice* beginAudio();
    void endAudio();

    // Only one plugin instance may consume the device->host ring.
    bool claimAudioOwner(const void* owner);
    void releaseAudioOwner(const void* owner);

    bool rename(const std::string& name);

    // Message channel of the running device (nullptr while not connected). Valid until the
    // connection generation changes; use under messengerLock().
    Messenger* messenger() { return messenger_.get(); }
    std::mutex& messengerLock() { return msgLock_; }
    // Incremented on every (re)connection, so clients can reload the device state.
    int generation() const { return generation_.load(); }
    // Overbridge protocol version the device reports (vendor request 4), 0 when unknown.
    // OS 1.10D: 127, OS 1.12: 129 (Overbridge 2.26.10 asks for 129 or later).
    int protocolVersion() const { return protocol_.load(); }

private:
    DeviceSession(uint16_t pid, std::string wantedId);
    void run();
    void setStatus(State s, const std::string& text);

    const uint16_t pid_;
    const std::string wantedId_;
    const DeviceDesc* desc_ = nullptr;

    std::atomic<int> protocol_{0};
    std::unique_ptr<UsbDevice> device_;
    std::unique_ptr<Messenger> messenger_;
    std::mutex msgLock_;
    std::atomic<int> generation_{0};
    std::atomic<State> state_{State::Searching};
    std::atomic<int> audioUsers_{0};
    std::atomic<bool> audioAllowed_{false};
    std::atomic<const void*> owner_{nullptr};

    mutable std::mutex lock_;
    std::string status_, name_, id_;
    std::vector<UsbDeviceInfo> available_;

    std::thread thread_;
    std::mutex wakeLock_;
    std::condition_variable wake_;
    bool quit_ = false;
};

} // namespace ob
