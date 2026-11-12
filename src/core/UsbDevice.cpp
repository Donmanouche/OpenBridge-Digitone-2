#include "UsbDevice.h"

#include <libusb.h>
#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ob {

namespace {
// wMaxPacketSize of an endpoint in an alternate setting (libusb_get_max_alt_packet_size exists only
// from libusb 1.0.27; Ubuntu 22.04 has 1.0.25). Returns a negative value when not found.
int maxAltPacketSize(libusb_device* dev, int interfaceNumber, int altSetting, unsigned char endpoint)
{
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != LIBUSB_SUCCESS || !cfg) return -1;
    int result = -1;
    for (int i = 0; i < cfg->bNumInterfaces && result < 0; ++i)
        for (int a = 0; a < cfg->interface[i].num_altsetting && result < 0; ++a) {
            const auto& alt = cfg->interface[i].altsetting[a];
            if (alt.bInterfaceNumber != interfaceNumber || alt.bAlternateSetting != altSetting) continue;
            for (int e = 0; e < alt.bNumEndpoints; ++e)
                if (alt.endpoint[e].bEndpointAddress == endpoint) result = alt.endpoint[e].wMaxPacketSize & 0x7ff;
        }
    libusb_free_config_descriptor(cfg);
    return result;
}
} // namespace


namespace {

constexpr int kConfiguration = 1;
constexpr int kAudioInInterface = 1;
constexpr int kAudioOutInterface = 2;
constexpr int kAudioAltSetting = 3;
constexpr unsigned char kAudioInEp = 0x83;
constexpr unsigned char kAudioOutEp = 0x03;
constexpr unsigned kTransferTimeoutMs = 10;
constexpr unsigned kControlTimeoutMs = 1000;
constexpr uint16_t kOutBlockHeader = 0x07ff;

constexpr float kInt32Scale = 1.0f / 2147483648.0f;

inline float decodeSample(const uint8_t* s, int bytes, StreamFormat f)
{
    int32_t v;
    if (bytes == 4) {
        if (f == StreamFormat::V2_1) // [ignored, hi, mid, lo]
            v = (int32_t)((uint32_t)s[1] << 24 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 8);
        else
            v = (int32_t)((uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3]);
    } else { // packed 24-bit big endian
        v = (int32_t)((uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8);
    }
    return (float)v * kInt32Scale;
}

inline void encodeSample(uint8_t* d, int bytes, StreamFormat f, float x)
{
    x = std::fmax(-1.0f, std::fmin(1.0f, x));
    int32_t v = (int32_t)std::lrint((double)x * 2147483647.0);
    if (bytes == 4) {
        if (f == StreamFormat::V2_1) v >>= 8; // right-aligned 24-bit
        d[0] = (uint8_t)(v >> 24);
        d[1] = (uint8_t)(v >> 16);
        d[2] = (uint8_t)(v >> 8);
        d[3] = (uint8_t)v;
    } else {
        d[0] = (uint8_t)(v >> 24);
        d[1] = (uint8_t)(v >> 16);
        d[2] = (uint8_t)(v >> 8);
    }
}

inline void updatePeak(std::atomic<float>& p, float v)
{
    v = std::fabs(v);
    float cur = p.load(std::memory_order_relaxed);
    while (v > cur && !p.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

} // namespace

std::string UsbDeviceInfo::id() const
{
    char buf[96];
    if (!serial.empty())
        std::snprintf(buf, sizeof buf, "%04x:%s", desc ? desc->pid : 0, serial.c_str());
    else
        std::snprintf(buf, sizeof buf, "%04x@%03u,%03u", desc ? desc->pid : 0, bus, address);
    return buf;
}

UsbDevice::UsbDevice()
{
    for (auto& p : stats_.toHostPeak) p.store(0.0f);
    for (auto& p : stats_.toDevicePeak) p.store(0.0f);
}

UsbDevice::~UsbDevice() { close(); }

std::vector<UsbDeviceInfo> UsbDevice::enumerate(std::string* error)
{
    std::vector<UsbDeviceInfo> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != LIBUSB_SUCCESS) {
        if (error) *error = "libusb_init failed";
        return out;
    }
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != LIBUSB_SUCCESS) continue;
        const DeviceDesc* desc = findDeviceDesc(dd.idVendor, dd.idProduct);
        if (!desc) continue;
        UsbDeviceInfo info;
        info.desc = desc;
        info.bus = libusb_get_bus_number(list[i]);
        info.address = libusb_get_device_address(list[i]);
        libusb_device_handle* h = nullptr;
        if (dd.iSerialNumber && libusb_open(list[i], &h) == LIBUSB_SUCCESS) {
            unsigned char s[128] = {};
            if (libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, s, sizeof s - 1) > 0)
                info.serial = (const char*)s;
            libusb_close(h);
        } else if (dd.iSerialNumber && error) {
            *error = "cannot open the device (check the udev rule for vendor 1935)";
        }
        out.push_back(info);
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

bool UsbDevice::open(const UsbDeviceInfo& wanted, std::string& error)
{
    close();
    if (libusb_init(&ctx_) != LIBUSB_SUCCESS) {
        error = "libusb_init failed";
        ctx_ = nullptr;
        return false;
    }
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx_, &list);
    for (ssize_t i = 0; i < n && !handle_; ++i) {
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != LIBUSB_SUCCESS) continue;
        if (!findDeviceDesc(dd.idVendor, dd.idProduct) || dd.idProduct != wanted.desc->pid) continue;
        if (libusb_get_bus_number(list[i]) != wanted.bus || libusb_get_device_address(list[i]) != wanted.address)
            continue;
        const int r = libusb_open(list[i], &handle_);
        if (r != LIBUSB_SUCCESS) {
            error = std::string("cannot open device: ") + libusb_error_name(r);
            handle_ = nullptr;
        }
    }
    if (list) libusb_free_device_list(list, 1);
    if (!handle_) {
        if (error.empty()) error = "device not found";
        libusb_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }
    info_ = wanted;
    libusb_set_auto_detach_kernel_driver(handle_, 1);
    int cfg = 0;
    if (libusb_get_configuration(handle_, &cfg) == LIBUSB_SUCCESS && cfg != kConfiguration) {
        const int r = libusb_set_configuration(handle_, kConfiguration);
        if (r != LIBUSB_SUCCESS) {
            error = std::string("cannot set configuration: ") + libusb_error_name(r);
            close();
            return false;
        }
    }
    return true;
}

void UsbDevice::close()
{
    stopAudio();
    if (handle_) {
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

bool UsbDevice::vendorIn(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len, int& got)
{
    if (!handle_) return false;
    got = libusb_control_transfer(handle_, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                  request, value, index, data, len, kControlTimeoutMs);
    return got >= 0;
}

bool UsbDevice::readName(std::string& name)
{
    uint8_t buf[33] = {};
    int got = 0;
    if (!vendorIn(1, 0, 0, buf, 32, got)) return false;
    name.assign((const char*)buf, strnlen((const char*)buf, (size_t)got));
    return true;
}

bool UsbDevice::writeName(const std::string& name)
{
    if (!handle_) return false;
    uint8_t buf[32] = {};
    std::memcpy(buf, name.data(), std::min<size_t>(name.size(), 31));
    const int r = libusb_control_transfer(handle_, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                          1, 0, 0, buf, 32, kControlTimeoutMs);
    return r == 32;
}

bool UsbDevice::readProtocolVersions(ProtocolVersions& v)
{
    uint8_t buf[4] = {};
    int got = 0;
    if (!vendorIn(4, 0, 0, buf, 4, got) || got != 4) return false;
    v.a = (uint16_t)(buf[0] << 8 | buf[1]);
    v.b = (uint16_t)(buf[2] << 8 | buf[3]);
    return true;
}

std::string UsbDevice::lastError() const
{
    std::lock_guard<std::mutex> l(errorLock_);
    return lastError_;
}

void UsbDevice::setError(const std::string& e)
{
    std::lock_guard<std::mutex> l(errorLock_);
    lastError_ = e;
}

bool UsbDevice::startAudio(int blocksPerTransfer, std::string& error)
{
    if (!handle_) {
        error = "device not open";
        return false;
    }
    stopAudio();
    blocksPerTransfer_ = std::max(2, std::min(blocksPerTransfer, 32));

    auto fail = [&](const char* what, int r) {
        error = std::string(what) + ": " + libusb_error_name(r);
        setError(error);
        return false;
    };
    int r;
    if ((r = libusb_claim_interface(handle_, kAudioInInterface)) != LIBUSB_SUCCESS) return fail("claim audio in", r);
    // Restart both streams from a known state: a previous client may have been killed mid-stream.
    libusb_set_interface_alt_setting(handle_, kAudioInInterface, 0);
    if ((r = libusb_set_interface_alt_setting(handle_, kAudioInInterface, kAudioAltSetting)) != LIBUSB_SUCCESS)
        return fail("alt setting audio in", r);
    if ((r = libusb_claim_interface(handle_, kAudioOutInterface)) != LIBUSB_SUCCESS) return fail("claim audio out", r);
    libusb_set_interface_alt_setting(handle_, kAudioOutInterface, 0);
    if ((r = libusb_set_interface_alt_setting(handle_, kAudioOutInterface, kAudioAltSetting)) != LIBUSB_SUCCESS)
        return fail("alt setting audio out", r);
    libusb_clear_halt(handle_, kAudioInEp);
    libusb_clear_halt(handle_, kAudioOutEp);

    const auto& d = desc();
    inBlockLen_ = kBlockHeaderBytes + kFramesPerBlock * d.toHostFrameBytes();
    outBlockLen_ = kBlockHeaderBytes + kFramesPerBlock * d.toDeviceFrameBytes();
    // Cross-check with the endpoint descriptor when libusb can tell us.
    if (const int mps = maxAltPacketSize(libusb_get_device(handle_), kAudioInInterface, kAudioAltSetting, kAudioInEp);
        mps > 0 && mps != inBlockLen_) {
        error = "unexpected audio IN block size " + std::to_string(mps) + " (expected " + std::to_string(inBlockLen_) + ")";
        setError(error);
        return false;
    }

    const int frames = framesPerTransfer();
    inBuf_.assign((size_t)inBlockLen_ * blocksPerTransfer_, 0);
    outBuf_.assign((size_t)outBlockLen_ * blocksPerTransfer_, 0);
    for (int b = 0; b < blocksPerTransfer_; ++b) {
        uint8_t* blk = &outBuf_[(size_t)b * outBlockLen_];
        blk[0] = kOutBlockHeader >> 8;
        blk[1] = kOutBlockHeader & 0xff;
    }
    inFrames_.assign((size_t)frames * d.toHost.size(), 0.0f);
    outFrames_.assign((size_t)frames * d.toDevice.size(), 0.0f);
    toHost_.configure((int)d.toHost.size(), 48000 / 2);
    toDevice_.configure((int)d.toDevice.size(), 48000 / 2);
    outFrameCounter_ = 0;

    inXfr_ = libusb_alloc_transfer(0);
    outXfr_ = libusb_alloc_transfer(0);
    stopRequested_ = false;
    failed_ = false;
    running_ = true;
    pending_ = 0;
    submitIn();
    encodeOut(outBuf_.data());
    submitOut();
    thread_ = std::thread([this] { eventLoop(); });
    return true;
}

void UsbDevice::stopAudio()
{
    if (!running_.load() && !thread_.joinable()) return;
    stopRequested_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
    if (inXfr_) libusb_free_transfer(inXfr_);
    if (outXfr_) libusb_free_transfer(outXfr_);
    inXfr_ = outXfr_ = nullptr;
    if (handle_) { // close the streams cleanly: back to the zero-bandwidth alternate setting
        libusb_set_interface_alt_setting(handle_, kAudioInInterface, 0);
        libusb_set_interface_alt_setting(handle_, kAudioOutInterface, 0);
        libusb_release_interface(handle_, kAudioInInterface);
        libusb_release_interface(handle_, kAudioOutInterface);
    }
}

void UsbDevice::eventLoop()
{
    sched_param sp{};
    sp.sched_priority = 10; // below typical DAW audio threads
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); // best effort (needs rtprio rights)
    pthread_setname_np(pthread_self(), "ob-usb");

    while (pending_.load() > 0) {
        timeval tv{0, 100000};
        if (stopRequested_.load()) {
            if (inXfr_) libusb_cancel_transfer(inXfr_);
            if (outXfr_) libusb_cancel_transfer(outXfr_);
        }
        const int r = libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
        if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_INTERRUPTED) {
            setError(std::string("libusb events: ") + libusb_error_name(r));
            failed_ = true;
            break;
        }
    }
    running_ = false;
}

void UsbDevice::submitIn()
{
    libusb_fill_interrupt_transfer(inXfr_, handle_, kAudioInEp, inBuf_.data(), (int)inBuf_.size(), &UsbDevice::inCallback,
                                   this, kTransferTimeoutMs);
    if (libusb_submit_transfer(inXfr_) == LIBUSB_SUCCESS)
        ++pending_;
    else {
        failed_ = true;
        setError("cannot submit audio IN transfer");
    }
}

void UsbDevice::submitOut()
{
    libusb_fill_interrupt_transfer(outXfr_, handle_, kAudioOutEp, outBuf_.data(), (int)outBuf_.size(),
                                   &UsbDevice::outCallback, this, kTransferTimeoutMs);
    if (libusb_submit_transfer(outXfr_) == LIBUSB_SUCCESS)
        ++pending_;
    else {
        failed_ = true;
        setError("cannot submit audio OUT transfer");
    }
}

void UsbDevice::inCallback(libusb_transfer* t)
{
    auto* self = static_cast<UsbDevice*>(t->user_data);
    --self->pending_;
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        ++self->stats_.inTransfers;
        self->decodeIn(t->buffer, t->actual_length);
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        if (self->stats_.inErrors++ == 0)
            std::fprintf(stderr, "OpenBridge: audio IN transfer failed: %s\n", libusb_error_name(t->status));
        if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
            self->failed_ = true;
            self->setError("device disconnected");
            return;
        }
    }
    if (!self->stopRequested_.load() && !self->failed_.load()) self->submitIn();
}

void UsbDevice::outCallback(libusb_transfer* t)
{
    auto* self = static_cast<UsbDevice*>(t->user_data);
    --self->pending_;
    if (t->status == LIBUSB_TRANSFER_COMPLETED)
        ++self->stats_.outTransfers;
    else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        if (self->stats_.outErrors++ == 0)
            std::fprintf(stderr, "OpenBridge: audio OUT transfer failed: %s (%d/%d bytes)\n",
                         libusb_error_name(t->status), t->actual_length, t->length);
        if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
            self->failed_ = true;
            self->setError("device disconnected");
            return;
        }
    }
    if (!self->stopRequested_.load() && !self->failed_.load()) {
        self->encodeOut(self->outBuf_.data());
        self->submitOut();
    }
}

void UsbDevice::decodeIn(const uint8_t* data, int len)
{
    const auto& d = desc();
    const int ch = (int)d.toHost.size();
    const int blocks = len / inBlockLen_;
    float* f = inFrames_.data();
    for (int b = 0; b < blocks; ++b) {
        const uint8_t* s = data + (size_t)b * inBlockLen_ + kBlockHeaderBytes;
        for (int fr = 0; fr < kFramesPerBlock; ++fr) {
            for (int c = 0; c < ch; ++c) {
                const int bytes = d.toHost[(size_t)c].bytes;
                const float v = decodeSample(s, bytes, d.format);
                *f++ = v;
                if (c < 64) updatePeak(stats_.toHostPeak[c], v);
                s += bytes;
            }
        }
    }
    const size_t frames = (size_t)blocks * kFramesPerBlock;
    if (toHost_.write(inFrames_.data(), frames) < frames) ++stats_.toHostOverflows;
}

void UsbDevice::encodeOut(uint8_t* data)
{
    const auto& d = desc();
    const int ch = (int)d.toDevice.size();
    const size_t frames = (size_t)framesPerTransfer();
    // All or nothing: a partial transfer would cut the stream in the middle of a period.
    if (toDevice_.readable() >= frames) {
        toDevice_.read(outFrames_.data(), frames);
    } else {
        if (toDevice_.readable() > 0) ++stats_.toDeviceUnderruns;
        std::fill(outFrames_.begin(), outFrames_.end(), 0.0f);
    }
    const float* f = outFrames_.data();
    for (int b = 0; b < blocksPerTransfer_; ++b) {
        uint8_t* blk = data + (size_t)b * outBlockLen_;
        blk[2] = (uint8_t)(outFrameCounter_ >> 8);
        blk[3] = (uint8_t)outFrameCounter_;
        outFrameCounter_ = (uint16_t)(outFrameCounter_ + kFramesPerBlock);
        uint8_t* s = blk + kBlockHeaderBytes;
        for (int fr = 0; fr < kFramesPerBlock; ++fr) {
            for (int c = 0; c < ch; ++c) {
                const int bytes = d.toDevice[(size_t)c].bytes;
                if (c < 16) updatePeak(stats_.toDevicePeak[c], *f);
                encodeSample(s, bytes, d.format, *f++);
                s += bytes;
            }
        }
    }
}

} // namespace ob
