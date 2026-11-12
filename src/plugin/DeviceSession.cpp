#include "DeviceSession.h"

#include <chrono>
#include <map>

namespace ob {

namespace {
std::mutex registryLock;
std::map<std::string, std::weak_ptr<DeviceSession>> registry;
} // namespace

std::shared_ptr<DeviceSession> DeviceSession::acquire(uint16_t pid, const std::string& wantedId)
{
    std::lock_guard<std::mutex> l(registryLock);
    const std::string key = std::to_string(pid) + "/" + wantedId;
    if (auto s = registry[key].lock()) return s;
    std::shared_ptr<DeviceSession> s(new DeviceSession(pid, wantedId));
    registry[key] = s;
    return s;
}

DeviceSession::DeviceSession(uint16_t pid, std::string wantedId) : pid_(pid), wantedId_(std::move(wantedId))
{
    desc_ = findDeviceDesc(kElektronVid, pid);
    status_ = "Searching for device...";
    thread_ = std::thread([this] { run(); });
}

DeviceSession::~DeviceSession()
{
    {
        std::lock_guard<std::mutex> l(wakeLock_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DeviceSession::setStatus(State s, const std::string& text)
{
    state_ = s;
    std::lock_guard<std::mutex> l(lock_);
    status_ = text;
}

std::string DeviceSession::statusText() const
{
    std::lock_guard<std::mutex> l(lock_);
    return status_;
}

std::string DeviceSession::deviceName() const
{
    std::lock_guard<std::mutex> l(lock_);
    return name_;
}

std::string DeviceSession::deviceId() const
{
    std::lock_guard<std::mutex> l(lock_);
    return id_;
}

std::vector<UsbDeviceInfo> DeviceSession::availableDevices() const
{
    std::lock_guard<std::mutex> l(lock_);
    return available_;
}

UsbDevice* DeviceSession::beginAudio()
{
    ++audioUsers_;
    if (!audioAllowed_.load(std::memory_order_acquire)) {
        --audioUsers_;
        return nullptr;
    }
    return device_.get();
}

void DeviceSession::endAudio() { --audioUsers_; }

bool DeviceSession::claimAudioOwner(const void* owner)
{
    const void* expected = nullptr;
    return owner_.compare_exchange_strong(expected, owner) || expected == owner;
}

void DeviceSession::releaseAudioOwner(const void* owner)
{
    const void* expected = owner;
    owner_.compare_exchange_strong(expected, nullptr);
}

bool DeviceSession::rename(const std::string& name)
{
    std::lock_guard<std::mutex> l(lock_);
    if (!device_ || state_.load() != State::Running) return false;
    if (!device_->writeName(name)) return false;
    name_ = name;
    return true;
}

void DeviceSession::run()
{
    auto waitFor = [this](std::chrono::milliseconds ms) {
        std::unique_lock<std::mutex> l(wakeLock_);
        return !wake_.wait_for(l, ms, [this] { return quit_; });
    };

    do {
        if (!device_) {
            std::string err;
            auto devs = UsbDevice::enumerate(&err);
            std::vector<UsbDeviceInfo> mine;
            for (auto& d : devs)
                if (d.desc->pid == pid_) mine.push_back(d);
            {
                std::lock_guard<std::mutex> l(lock_);
                available_ = mine;
            }
            const UsbDeviceInfo* pick = nullptr;
            for (auto& d : mine)
                if (wantedId_.empty() || d.id() == wantedId_) {
                    pick = &d;
                    break;
                }
            if (!pick) {
                setStatus(State::Searching, err.empty() ? "No " + std::string(desc_ ? desc_->name : "device") +
                                                              " found. Connect it and select OVERBRIDGE in USB CONFIG."
                                                        : err);
                continue;
            }
            setStatus(State::Connecting, "Connecting...");
            auto dev = std::make_unique<UsbDevice>();
            if (!dev->open(*pick, err)) {
                setStatus(State::Error, "Cannot open device: " + err);
                continue;
            }
            std::string name;
            dev->readName(name);
            {
                ProtocolVersions v;
                protocol_ = dev->readProtocolVersions(v) ? v.a : 0;
            }
            if (!dev->startAudio(24, err)) {
                setStatus(State::Error, "Cannot start audio: " + err);
                continue;
            }
            {
                std::lock_guard<std::mutex> l(lock_);
                name_ = name.empty() ? pick->desc->name : name;
                id_ = pick->id();
                device_ = std::move(dev);
            }
            {
                auto msg = std::make_unique<Messenger>();
                std::string merr;
                if (!msg->start(device_->handle(), merr)) {
                    setStatus(State::Running, "Connected (no message channel: " + merr + ")");
                } else {
                    std::lock_guard<std::mutex> l(msgLock_);
                    messenger_ = std::move(msg);
                }
            }
            ++generation_;
            audioAllowed_.store(true, std::memory_order_release);
            if (messenger_) setStatus(State::Running, "Connected");
        } else {
            // A device left mid-stream by a crashed client keeps refusing host->device audio
            // (every OUT transfer times out) until it is power-cycled.
            auto& st = device_->stats();
            const uint64_t ok = st.outTransfers.load(), bad = st.outErrors.load();
            const bool stuck = bad > 200 && ok < bad / 10;
            const std::string want = stuck ? "The Digitone does not accept audio from the computer: switch it off and on again"
                                           : "Connected";
            if ((stuck || statusText().rfind("The Digitone", 0) == 0) && statusText() != want) setStatus(State::Running, want);
        }
        if (device_ && device_->failed()) {
            const std::string why = device_->lastError();
            {
                std::lock_guard<std::mutex> l(msgLock_);
                if (messenger_) messenger_->stop();
                messenger_.reset();
            }
            audioAllowed_.store(false, std::memory_order_release);
            while (audioUsers_.load() > 0) std::this_thread::yield();
            std::unique_ptr<UsbDevice> old;
            {
                std::lock_guard<std::mutex> l(lock_);
                old = std::move(device_);
            }
            old.reset();
            setStatus(State::Searching, "Device lost (" + why + "), reconnecting...");
        }
    } while (waitFor(std::chrono::milliseconds(device_ ? 250 : 1000)));

    audioAllowed_.store(false, std::memory_order_release);
    while (audioUsers_.load() > 0) std::this_thread::yield();
    {
        std::lock_guard<std::mutex> l(msgLock_);
        if (messenger_) messenger_->stop();
        messenger_.reset();
    }
    std::lock_guard<std::mutex> l(lock_);
    device_.reset();
}

} // namespace ob
