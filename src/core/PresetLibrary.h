// +Drive preset library (banks A..H, 256 slots each) read in the background over SysEx.
// Listing a bank gives the names and slot numbers; the tags come from the preset files
// themselves, read one by one and kept in a cache file between sessions.
#pragma once

#include "SysexClient.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ob {

class PresetLibrary {
public:
    static constexpr int kBanks = 8;
    static constexpr int kSlots = 256;

    struct Preset {
        int bank = 0;   // 0..7 = A..H
        int slot = 0;   // 1..256
        std::string name;
        uint32_t tags = 0;
        bool tagsKnown = false;
        bool writeProtected = false;
        uint32_t size = 0;
    };

    PresetLibrary(std::string deviceName, std::string cacheFile);
    ~PresetLibrary();

    void start();
    void stop();
    // Re-lists every bank (e.g. after the device was reconnected or a preset was saved).
    void refresh();
    // The bank shown by the editor gets its tags read first.
    void setPriorityBank(int bank) { priority_ = bank; }

    // Changes whenever the content below changes.
    uint64_t generation() const { return generation_.load(); }
    bool bankListed(int bank) const;
    std::vector<Preset> bank(int bank) const;
    std::string status() const;

    // Reads a preset file in the background (before the tag reads); the callback runs on the
    // library thread with the sound container (the file without its 36-byte header).
    using ReadCallback = std::function<void(bool ok, std::vector<uint8_t> container)>;
    void readPreset(int bank, int slot, ReadCallback cb);

    // Device slot number used by the storage operations (class 0x43 / 0x44, type 0x27).
    static uint32_t deviceSlot(int bank, int slot) { return (uint32_t)(bank * kSlots + slot - 1); }

private:
    void run();
    void loadCache();
    void saveCache();
    void setStatus(std::string s);

    const std::string deviceName_, cacheFile_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> refresh_{true};
    std::atomic<int> priority_{0};
    std::atomic<uint64_t> generation_{1};

    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::array<std::vector<Preset>, kBanks> banks_;
    std::array<bool, kBanks> listed_{};
    std::string status_;
    struct CacheEntry {
        std::string key;
        uint32_t tags;
    };
    struct ReadJob {
        int bank, slot;
        ReadCallback cb;
    };
    std::deque<ReadJob> jobs_;
    std::vector<CacheEntry> cache_; // key "<bank>/<slot>/<size>/<name>"
    bool cacheDirty_ = false;
};

} // namespace ob
