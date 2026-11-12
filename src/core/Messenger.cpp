#include "Messenger.h"

#include <libusb.h>
#include <pthread.h>

#include <cstring>

namespace ob {

namespace {

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr int kHeartbeatMs = 500;
constexpr int kHeartbeatTimeoutMs = 3000;
constexpr int kReadTimeoutMs = 1500;
constexpr int kReadTries = 3;
constexpr int kMaxPacket = 512;

bool lz4Block(const uint8_t* s, size_t n, std::vector<uint8_t>& out)
{
    size_t i = 0;
    while (i < n) {
        const uint8_t tok = s[i++];
        size_t lit = tok >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = s[i++];
                lit += b;
            } while (b == 255);
        }
        if (i + lit > n) return false;
        out.insert(out.end(), s + i, s + i + lit);
        i += lit;
        if (i >= n) break; // last sequence has no match
        if (i + 2 > n) return false;
        const size_t off = s[i] | (size_t)s[i + 1] << 8;
        i += 2;
        size_t len = tok & 15;
        if (len == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = s[i++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (off == 0 || off > out.size()) return false;
        const size_t from = out.size() - off;
        for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
    }
    return true;
}

} // namespace

bool lz4DecodeFrame(const uint8_t* d, size_t n, std::vector<uint8_t>& out)
{
    out.clear();
    if (n < 7 || d[0] != 0x04 || d[1] != 0x22 || d[2] != 0x4d || d[3] != 0x18) return false;
    const uint8_t flg = d[4];
    size_t i = 6;
    if (flg & 0x08) i += 8; // content size
    if (flg & 0x01) i += 4; // dictionary id
    i += 1;                 // header checksum
    while (i + 4 <= n) {
        uint32_t bs;
        std::memcpy(&bs, d + i, 4); // little endian
        i += 4;
        if (bs == 0) return true;
        const size_t size = bs & 0x7fffffffu;
        if (i + size > n) return false;
        if (bs & 0x80000000u) out.insert(out.end(), d + i, d + i + size);
        else if (!lz4Block(d + i, size, out)) return false;
        i += size;
        if (flg & 0x10) i += 4; // block checksum
    }
    return true;
}

Messenger::~Messenger() { stop(); }

bool Messenger::start(libusb_device_handle* handle, std::string& error)
{
    stop();
    handle_ = handle;
    const int r = libusb_claim_interface(handle_, kMsgInterface);
    if (r != LIBUSB_SUCCESS) {
        error = std::string("claim message interface: ") + libusb_error_name(r);
        return false;
    }
    quit_ = false;
    running_ = true;
    rx_.clear();
    partials_.clear();
    lastHeartbeatReplyMs_ = 0;
    reader_ = std::thread([this] { readerLoop(); });
    writer_ = std::thread([this] { writerLoop(); });
    return true;
}

void Messenger::stop()
{
    if (!running_.load()) return;
    {
        std::lock_guard<std::mutex> l(lock_);
        quit_ = true;
    }
    wake_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (writer_.joinable()) writer_.join();
    if (handle_) libusb_release_interface(handle_, kMsgInterface);
    // fail outstanding reads
    std::map<uint32_t, PendingRead> inflight;
    std::vector<PendingRead> queued;
    {
        std::lock_guard<std::mutex> l(lock_);
        inflight.swap(inFlight_);
        queued.swap(readQueue_);
        paramQueue_.clear();
    }
    Message none;
    for (auto& [s, r] : inflight)
        if (r.cb) r.cb(false, none);
    for (auto& r : queued)
        if (r.cb) r.cb(false, none);
    running_ = false;
}

void Messenger::setNotificationHandler(std::function<void(const Message&)> fn)
{
    std::lock_guard<std::mutex> l(notifyLock_);
    onNotification_ = std::move(fn);
}

bool Messenger::alive() const
{
    const int64_t t = lastHeartbeatReplyMs_.load();
    return t != 0 && nowMs() - t < kHeartbeatTimeoutMs;
}

void Messenger::setParam(uint8_t object, uint16_t arg, uint16_t a, uint16_t pid, uint16_t value)
{
    const uint64_t key = (uint64_t)object << 48 | (uint64_t)arg << 32 | (uint64_t)a << 16 | pid;
    {
        std::lock_guard<std::mutex> l(lock_);
        paramQueue_[key] = makeAutomation(0, object, arg, a, pid, value); // seq set when sent
    }
    wake_.notify_all();
}

void Messenger::write(uint8_t cls, uint8_t type, uint16_t arg, std::vector<uint8_t> payload)
{
    {
        std::lock_guard<std::mutex> l(lock_);
        writeQueue_.push_back({cls, type, arg, std::move(payload)});
    }
    wake_.notify_all();
}

void Messenger::read(uint8_t type, uint16_t arg, ReadCallback cb, uint32_t p0, uint32_t p1)
{
    {
        std::lock_guard<std::mutex> l(lock_);
        readQueue_.push_back({type, arg, p0, p1, std::move(cb), 0, {}});
    }
    wake_.notify_all();
}

bool Messenger::send(const std::vector<uint8_t>& packet)
{
    int sent = 0;
    const int r = libusb_bulk_transfer(handle_, kMsgOutEp, const_cast<uint8_t*>(packet.data()), (int)packet.size(), &sent,
                                       500);
    if (r == LIBUSB_SUCCESS && packet.size() % kMaxPacket == 0) {
        int zlp = 0;
        libusb_bulk_transfer(handle_, kMsgOutEp, nullptr, 0, &zlp, 100);
    }
    if (r == LIBUSB_SUCCESS) ++messagesOut_;
    return r == LIBUSB_SUCCESS && sent == (int)packet.size();
}

void Messenger::writerLoop()
{
    pthread_setname_np(pthread_self(), "ob-msg-tx");
    int64_t nextHeartbeat = 0;
    std::unique_lock<std::mutex> l(lock_);
    while (!quit_) {
        const int64_t now = nowMs();
        if (now >= nextHeartbeat) {
            const uint16_t s = ++hbSeq_;
            l.unlock();
            send({0xFF, 0xAA, (uint8_t)(s >> 8), (uint8_t)s});
            l.lock();
            nextHeartbeat = now + kHeartbeatMs;
        }
        // parameter changes
        if (!paramQueue_.empty()) {
            std::map<uint64_t, std::vector<uint8_t>> batch;
            batch.swap(paramQueue_);
            std::vector<std::vector<uint8_t>> packets;
            for (auto& [k, p] : batch) {
                const uint32_t s = seq_++;
                p[2] = (uint8_t)(s >> 24);
                p[3] = (uint8_t)(s >> 16);
                p[4] = (uint8_t)(s >> 8);
                p[5] = (uint8_t)s;
                packets.push_back(std::move(p));
            }
            l.unlock();
            // several messages may share one USB transfer
            std::vector<uint8_t> chunk;
            for (auto& p : packets) {
                if (chunk.size() + p.size() > 1023) {
                    send(chunk);
                    chunk.clear();
                }
                chunk.insert(chunk.end(), p.begin(), p.end());
            }
            if (!chunk.empty()) send(chunk);
            l.lock();
        }
        // generic writes, in order
        while (!writeQueue_.empty()) {
            PendingWrite w = std::move(writeQueue_.front());
            writeQueue_.erase(writeQueue_.begin());
            const uint32_t s = seq_++;
            auto pkt = makeTransaction(kAutomationChannel, s, w.cls, w.type, w.arg, w.payload.data(), (uint16_t)w.payload.size());
            l.unlock();
            send(pkt);
            l.lock();
        }
        // read requests: retries of timed out ones, then new ones (a few in flight at a time)
        const auto tnow = std::chrono::steady_clock::now();
        for (auto it = inFlight_.begin(); it != inFlight_.end();) {
            if (tnow - it->second.sent > std::chrono::milliseconds(kReadTimeoutMs)) {
                PendingRead r = std::move(it->second);
                it = inFlight_.erase(it);
                if (r.tries < kReadTries) readQueue_.insert(readQueue_.begin(), std::move(r));
                else if (r.cb) {
                    auto cb = std::move(r.cb);
                    l.unlock();
                    cb(false, Message{});
                    l.lock();
                }
            } else
                ++it;
        }
        while (!readQueue_.empty() && inFlight_.size() < 4) {
            PendingRead r = std::move(readQueue_.front());
            readQueue_.erase(readQueue_.begin());
            const uint32_t s = seq_++;
            uint8_t payload[8];
            for (int i = 0; i < 4; ++i) {
                payload[i] = (uint8_t)(r.p0 >> (24 - 8 * i));
                payload[4 + i] = (uint8_t)(r.p1 >> (24 - 8 * i));
            }
            auto pkt = makeTransaction(kAutomationChannel, s, 0x20, r.type, r.arg, payload, 8);
            r.sent = std::chrono::steady_clock::now();
            ++r.tries;
            inFlight_[s] = std::move(r);
            l.unlock();
            send(pkt);
            l.lock();
        }
        wake_.wait_for(l, std::chrono::milliseconds(5));
    }
}

void Messenger::readerLoop()
{
    pthread_setname_np(pthread_self(), "ob-msg-rx");
    uint8_t buf[64];
    while (!quit_.load()) {
        int got = 0;
        const int r = libusb_interrupt_transfer(handle_, kMsgInEp, buf, sizeof buf, &got, 50);
        if (r == LIBUSB_SUCCESS && got > 0) {
            rx_.insert(rx_.end(), buf, buf + got);
            parse();
        } else if (r == LIBUSB_ERROR_NO_DEVICE) {
            break;
        } else if (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_SUCCESS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // never spin on errors
        }
    }
}

void Messenger::parse()
{
    size_t i = 0;
    while (true) {
        const size_t n = rx_.size() - i;
        if (n < 4) break;
        const uint8_t* p = rx_.data() + i;
        if (p[0] == 0x00 && p[1] == 0x55) { // heartbeat answer
            lastHeartbeatReplyMs_ = nowMs();
            i += 4;
            continue;
        }
        if (p[1] != kMsgVersion) { // lost sync: skip a byte
            ++i;
            continue;
        }
        if (n < (size_t)kMsgHeaderBytes) break;
        MsgHeader h;
        MsgHeader::read(p, h);
        if (n < (size_t)kMsgHeaderBytes + h.chunkLength) break;
        const uint8_t* data = p + kMsgHeaderBytes;
        i += kMsgHeaderBytes + h.chunkLength;
        ++messagesIn_;

        if (h.chunkLength == h.totalLength && h.offset == 0) {
            Message m{h, std::vector<uint8_t>(data, data + h.chunkLength)};
            deliver(std::move(m));
            continue;
        }
        const uint64_t key = (uint64_t)h.channel << 32 | h.seq;
        auto& part = partials_[key];
        if (h.offset == 0) {
            part.header = h;
            part.data.clear();
        }
        if (part.data.size() != h.offset) { // lost a chunk
            partials_.erase(key);
            continue;
        }
        part.data.insert(part.data.end(), data, data + h.chunkLength);
        if (part.data.size() >= h.totalLength) {
            Message m{part.header, std::move(part.data)};
            partials_.erase(key);
            deliver(std::move(m));
        }
    }
    rx_.erase(rx_.begin(), rx_.begin() + (ptrdiff_t)i);
}

void Messenger::deliver(Message&& m)
{
    if (m.header.compressed) {
        std::vector<uint8_t> out;
        if (!lz4DecodeFrame(m.payload.data(), m.payload.size(), out)) return;
        m.payload.swap(out);
        m.header.compressed = false;
        m.header.totalLength = (uint32_t)m.payload.size();
    }
    // replies carry the request's sequence number in bytes 6..9
    ReadCallback cb;
    if (m.header.cls == 0x21 || m.header.cls == 0x23) {
        std::lock_guard<std::mutex> l(lock_);
        auto it = inFlight_.find(m.header.reserved);
        if (it != inFlight_.end()) {
            cb = std::move(it->second.cb);
            inFlight_.erase(it);
        }
    }
    if (cb) {
        cb(true, m);
        return;
    }
    std::lock_guard<std::mutex> l(notifyLock_);
    if (onNotification_) onNotification_(m);
}

} // namespace ob
