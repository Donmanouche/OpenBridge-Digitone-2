// Overbridge message channel on USB interface 3:
// heartbeat, coalesced parameter writes, read requests with reassembly / LZ4, incoming notifications.
#pragma once

#include "Messages.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct libusb_device_handle;

namespace ob {

// Decodes an LZ4 frame (as sent by the device). Returns false on malformed input.
bool lz4DecodeFrame(const uint8_t* src, size_t n, std::vector<uint8_t>& out);

struct Message {
    MsgHeader header;
    std::vector<uint8_t> payload; // complete, decompressed
};

class Messenger {
public:
    using ReadCallback = std::function<void(bool ok, const Message&)>;

    Messenger() = default;
    ~Messenger();

    bool start(libusb_device_handle* handle, std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    // True while the device answers heartbeats.
    bool alive() const;

    // One parameter change (automation message). Successive writes to the same parameter
    // before the next flush keep only the last value.
    void setParam(uint8_t object, uint16_t arg, uint16_t a, uint16_t pid, uint16_t value);

    // Generic transaction (e.g. class 0x10 type 6: write a sound setup). Sent in order.
    void write(uint8_t cls, uint8_t type, uint16_t arg, std::vector<uint8_t> payload);

    // Read request (class 0x20). The callback runs on the reader thread.
    void read(uint8_t type, uint16_t arg, ReadCallback cb, uint32_t p0 = 0, uint32_t p1 = 0);

    // Every complete message from the device that is not a reply to read() (notifications).
    // Runs on the reader thread. Pass nullptr to unsubscribe (returns after any running call).
    void setNotificationHandler(std::function<void(const Message&)> fn);

    uint64_t messagesIn() const { return messagesIn_.load(); }
    uint64_t messagesOut() const { return messagesOut_.load(); }

private:
    struct PendingRead {
        uint8_t type;
        uint16_t arg;
        uint32_t p0, p1;
        ReadCallback cb;
        int tries = 0;
        std::chrono::steady_clock::time_point sent;
    };
    struct Partial {
        MsgHeader header;
        std::vector<uint8_t> data;
    };

    void readerLoop();
    void writerLoop();
    bool send(const std::vector<uint8_t>& packet);
    void parse();
    void deliver(Message&& m);

    libusb_device_handle* handle_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::thread reader_, writer_;

    std::mutex lock_;
    std::condition_variable wake_;
    std::map<uint64_t, std::vector<uint8_t>> paramQueue_; // key (object,arg,a,pid) -> automation packet
    std::vector<PendingRead> readQueue_;                   // not yet sent
    struct PendingWrite {
        uint8_t cls, type;
        uint16_t arg;
        std::vector<uint8_t> payload;
    };
    std::vector<PendingWrite> writeQueue_;
    std::map<uint32_t, PendingRead> inFlight_;             // seq -> request
    uint32_t seq_ = 1;
    uint16_t hbSeq_ = 0;
    std::atomic<int64_t> lastHeartbeatReplyMs_{0};

    std::mutex notifyLock_;
    std::function<void(const Message&)> onNotification_;
    std::vector<uint8_t> rx_;                    // reader thread only
    std::map<uint64_t, Partial> partials_;       // reader thread only
    std::atomic<uint64_t> messagesIn_{0}, messagesOut_{0};
};

} // namespace ob
