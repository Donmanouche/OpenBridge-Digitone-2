#include "PresetLibrary.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ob {

namespace {
std::string cacheKey(int bank, int slot, uint32_t size, const std::string& name)
{
    return std::to_string(bank) + "/" + std::to_string(slot) + "/" + std::to_string(size) + "/" + name;
}
} // namespace

PresetLibrary::PresetLibrary(std::string deviceName, std::string cacheFile)
    : deviceName_(std::move(deviceName)), cacheFile_(std::move(cacheFile))
{
}

PresetLibrary::~PresetLibrary() { stop(); }

void PresetLibrary::start()
{
    if (thread_.joinable()) return;
    quit_ = false;
    loadCache();
    thread_ = std::thread([this] { run(); });
}

void PresetLibrary::stop()
{
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PresetLibrary::refresh()
{
    refresh_ = true;
    wake_.notify_all();
}

bool PresetLibrary::bankListed(int bank) const
{
    std::lock_guard<std::mutex> g(lock_);
    return bank >= 0 && bank < kBanks && listed_[(size_t)bank];
}

std::vector<PresetLibrary::Preset> PresetLibrary::bank(int bank) const
{
    std::lock_guard<std::mutex> g(lock_);
    if (bank < 0 || bank >= kBanks) return {};
    return banks_[(size_t)bank];
}

std::string PresetLibrary::status() const
{
    std::lock_guard<std::mutex> g(lock_);
    return status_;
}

void PresetLibrary::setStatus(std::string s)
{
    std::lock_guard<std::mutex> g(lock_);
    if (s == status_) return;
    status_ = std::move(s);
    ++generation_;
}

void PresetLibrary::readPreset(int bank, int slot, ReadCallback cb)
{
    {
        std::lock_guard<std::mutex> g(lock_);
        jobs_.push_back({bank, slot, std::move(cb)});
    }
    wake_.notify_all();
}

void PresetLibrary::loadCache()
{
    std::ifstream in(cacheFile_);
    std::string line;
    std::lock_guard<std::mutex> g(lock_);
    cache_.clear();
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        cache_.push_back({line.substr(tab + 1), (uint32_t)std::strtoul(line.c_str(), nullptr, 16)});
    }
}

void PresetLibrary::saveCache()
{
    std::vector<CacheEntry> copy;
    {
        std::lock_guard<std::mutex> g(lock_);
        if (!cacheDirty_) return;
        cacheDirty_ = false;
        copy = cache_;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cacheFile_).parent_path(), ec);
    const std::string tmp = cacheFile_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        char hex[16];
        for (auto& e : copy) {
            std::snprintf(hex, sizeof hex, "%08x", e.tags);
            out << hex << '\t' << e.key << '\n';
        }
    }
    std::filesystem::rename(tmp, cacheFile_, ec);
}

void PresetLibrary::run()
{
    SysexClient client;
    auto sleepFor = [this](int ms) {
        std::unique_lock<std::mutex> g(lock_);
        wake_.wait_for(g, std::chrono::milliseconds(ms), [this] { return quit_.load() || refresh_.load() || !jobs_.empty(); });
    };
    while (!quit_) {
        if (!client.isOpen()) {
            std::string err;
            if (!client.open(deviceName_, err)) {
                setStatus(err);
                sleepFor(2000);
                continue;
            }
            refresh_ = true;
        }

        if (refresh_.exchange(false)) {
            setStatus("Reading the +Drive preset library...");
            bool failed = false;
            for (int b = 0; b < kBanks && !quit_; ++b) {
                std::vector<FsEntry> entries;
                const std::string path = std::string("/soundbanks/") + (char)('A' + b);
                if (!client.listDir(path, entries)) {
                    failed = true;
                    break;
                }
                std::vector<Preset> list;
                for (auto& e : entries) {
                    if (e.directory || e.id < 1 || e.id > kSlots) continue;
                    Preset p;
                    p.bank = b;
                    p.slot = e.id;
                    p.name = e.name;
                    p.size = e.size;
                    p.writeProtected = e.writeProtected();
                    list.push_back(std::move(p));
                }
                std::lock_guard<std::mutex> g(lock_);
                for (auto& p : list) {
                    const auto key = cacheKey(b, p.slot, p.size, p.name);
                    for (auto& c : cache_)
                        if (c.key == key) {
                            p.tags = c.tags;
                            p.tagsKnown = true;
                            break;
                        }
                }
                banks_[(size_t)b] = std::move(list);
                listed_[(size_t)b] = true;
                ++generation_;
            }
            if (failed) {
                client.close();
                setStatus("The +Drive library could not be read (device busy or disconnected)");
                sleepFor(2000);
                continue;
            }
            setStatus("");
        }

        // Requested preset reads first.
        ReadJob job{-1, 0, nullptr};
        {
            std::lock_guard<std::mutex> g(lock_);
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
        }
        if (job.cb) {
            std::vector<uint8_t> file;
            const std::string path = std::string("/soundbanks/") + (char)('A' + job.bank) + "/" + std::to_string(job.slot);
            PresetInfo info;
            const bool ok = client.readFile(path, file) && parsePresetFile(file, info);
            if (ok) file.erase(file.begin(), file.begin() + 36);
            job.cb(ok, ok ? std::move(file) : std::vector<uint8_t>{});
            continue;
        }

        // Read the tags of one preset whose tags are unknown, the shown bank first.
        int bank = -1, slot = 0;
        std::string name;
        uint32_t size = 0;
        {
            std::lock_guard<std::mutex> g(lock_);
            const int first = priority_.load();
            for (int k = 0; k < kBanks && bank < 0; ++k) {
                const int b = (first + k) % kBanks;
                for (auto& p : banks_[(size_t)b])
                    if (!p.tagsKnown) {
                        bank = b;
                        slot = p.slot;
                        name = p.name;
                        size = p.size;
                        break;
                    }
            }
        }
        if (bank < 0) {
            saveCache();
            sleepFor(60000);
            continue;
        }
        std::vector<uint8_t> file;
        PresetInfo info;
        const std::string path = std::string("/soundbanks/") + (char)('A' + bank) + "/" + std::to_string(slot);
        const bool readOk = client.readFile(path, file);
        const bool ok = readOk && parsePresetFile(file, info);
        {
            std::lock_guard<std::mutex> g(lock_);
            for (auto& p : banks_[(size_t)bank])
                if (p.slot == slot) {
                    p.tagsKnown = true; // also on failure: do not retry endlessly
                    if (ok) p.tags = info.tags;
                }
            if (ok) {
                cache_.push_back({cacheKey(bank, slot, size, name), info.tags});
                cacheDirty_ = true;
            }
            ++generation_;
        }
        if (!readOk) {
            client.close(); // reopen: the device may have gone away
            sleepFor(500);
        }
    }
    saveCache();
}

} // namespace ob
