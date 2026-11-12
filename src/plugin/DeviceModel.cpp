#include "DeviceModel.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>
#include <cstdio>
#include <cstring>

namespace ob {

namespace {

// Automation / notification object codes.
bool isPatternMute(const ParamDef& p) { return std::strcmp(p.key, "Kit_PatternTrackMute") == 0; }
constexpr uint8_t kObjSound = 5, kObjFx = 9, kObjKit = 0x0f, kObjKitPerTrack = 0x2d, kObjSettings = 0x1b;
constexpr uint8_t kReadSound = 0x04;
constexpr uint8_t kReadSetup = 0x06;
// Kit setup (80 bytes) and kit voice configuration (65 bytes), read and written with the same type.
// Protocol 129 (OS 1.12): 0x10 / 0x11. Protocol 127 (OS 1.10D): one lower.
uint8_t kitSetupType(int protocol) { return protocol >= 129 ? 0x10 : 0x0F; }
uint8_t kitVoiceConfigType(int protocol) { return protocol >= 129 ? 0x11 : 0x10; }
constexpr size_t kVoiceConfigBytes = 65, kKitSetupBytes = 80;   // sound setup, 125 bytes: track u16, tags u32, name[16], machine, filter ...
constexpr size_t kSetupBytes = 125;
constexpr int kSetupMachineOffset = 22;
constexpr int kSetupFilterOffset = 23;
constexpr int kSetupTagsOffset = 2;
constexpr int kSetupNameOffset = 6;
// Bytes of the sound container outside the u16 slots (measured on the device).
struct ByteParam {
    const char* key;
    int offset;
};
constexpr ByteParam kByteParams[] = {{"Sound_DistSampleRateReductionRouting", 0xEA},
                                     {"Sound_FilterBaseWidthRouting", 0xEE},
                                     {"Sound_DistDistortionRouting", 0xF0}};

// Sound container slot -> modulation destination menu index (LFO / mod source destinations).
// Measured on the device by writing each menu index and reading the container back.
// -1: not a destination.
constexpr int kSlotToMenu[107] = {
    0,  1,  9,  17, 0,  2,  10, 18, 0,  3,  11, 19, 0,  4,  12, 20, 0,  5,  13, 21, 0,  6,  14, 22, 0,  7,  15,
    23, 0,  8,  16, 24, 0,  25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68, 69, 70, 71, 72, 73,
    74, 79, 76, 77, 75, 80, 81, 82, 83, 84, 85, 88, 86, 87, 89, 90, 91, 92, 94, 93, 95, 96, 97, 98, 78, 99};

int lfoDestinationValue(int lfo, int storedSlot)
{
    if (lfo < 0 || lfo > 2 || storedSlot < 0 || storedSlot >= 107) return -1;
    if (storedSlot == 0) return 0;
    const int idx = kSlotToMenu[storedSlot];
    if (idx == 0) return -1;
    // an LFO can only modulate the LFOs before it (measured: LFO2 -> LFO1, LFO3 -> LFO1 and LFO2)
    if (idx >= 1 && idx <= 24 && (idx - 1) / 8 >= lfo) return -1;
    return idx;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}


} // namespace

std::shared_ptr<DeviceModel> DeviceModel::create(std::shared_ptr<DeviceSession> session)
{
    return std::shared_ptr<DeviceModel>(new DeviceModel(std::move(session)));
}

DeviceModel::DeviceModel(std::shared_ptr<DeviceSession> session) : session_(std::move(session))
{
    const auto& params = digitone2Params();
    values_ = std::vector<std::atomic<int>>(params.size() * kTracks);
    dirty_ = std::vector<std::atomic<uint8_t>>(params.size() * kTracks);
    for (size_t i = 0; i < params.size(); ++i)
        for (int t = 0; t < kTracks; ++t) values_[i * kTracks + (size_t)t].store(params[i].def);
    for (int t = 0; t < kTracks; ++t) {
        machine_[(size_t)t] = -1;
        filter_[(size_t)t] = -1;
        loaded_[(size_t)t] = false;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        auto& v = byPid_[(int)params[i].object];
        if (params[i].pid < 0) continue;
        if ((int)v.size() <= params[i].pid) v.resize((size_t)params[i].pid + 1);
        v[(size_t)params[i].pid].push_back((int)i);
    }
    writer_ = std::thread([this] { writerLoop(); });
}

DeviceModel::~DeviceModel()
{
    quit_ = true;
    wake_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (!session_) return;
    std::lock_guard<std::mutex> l(session_->messengerLock());
    if (auto* m = session_->messenger()) {
        m->setNotificationHandler(nullptr);
        releaseSoloLocked(m);
    }
}

void DeviceModel::releaseSoloLocked(Messenger* m)
{
    // Solo exists only in the plugin: when it closes, the device keeps the mutes set by hand only.
    const auto* soloDef = findParam("Settings_TrackSolo");
    if (!soloDef) return;
    const int solo = (int)(soloDef - digitone2Params().data());
    std::vector<int> soloed;
    for (int t = 0; t < kTracks; ++t)
        if (get(solo, t) > 0) soloed.push_back(t);
    if (soloed.empty()) return;
    for (int t : soloed) values_[(size_t)slotIndex(solo, t)].store(0);
    std::lock_guard<std::mutex> l(midiSenderLock_);
    sendTrackMutesLocked(m);
}

void DeviceModel::setSession(std::shared_ptr<DeviceSession> session)
{
    if (session_) {
        std::lock_guard<std::mutex> l(session_->messengerLock());
        if (auto* m = session_->messenger()) m->setNotificationHandler(nullptr);
    }
    session_ = std::move(session);
    generation_ = -1;
    for (auto& l : loaded_) l = false;
}

bool DeviceModel::connected() const
{
    if (!session_) return false;
    std::lock_guard<std::mutex> l(session_->messengerLock());
    auto* m = session_->messenger();
    return m && m->alive();
}

std::string DeviceModel::soundName(int track) const
{
    std::lock_guard<std::mutex> l(nameLock_);
    return names_[(size_t)track];
}

void DeviceModel::poll()
{
    if (!session_) return;
    const int g = session_->generation();
    const int64_t now = nowMs();
    for (int t = 0; t < kTracks; ++t) {
        int64_t at = reloadAtMs_[(size_t)t].load();
        if (at && now >= at && reloadAtMs_[(size_t)t].compare_exchange_strong(at, 0))
            pendingReload_ |= 1u << t;
    }
    if (g == generation_ && pendingReload_.load() == 0) return;
    std::lock_guard<std::mutex> l(session_->messengerLock());
    auto* m = session_->messenger();
    if (!m) return;
    if (g != generation_) {
        generation_ = g;
        attach(m);
    }
    if (const uint32_t r = pendingReload_.exchange(0))
    {
        for (int t = 0; t < kTracks; ++t)
            if (r & (1u << t)) loadTrack(m, t);
        if (r == 0xffffu) loadKit(m);
    }
}

void DeviceModel::attach(Messenger* m)
{
    std::weak_ptr<DeviceModel> weak = shared_from_this();
    m->setNotificationHandler([weak](const Message& msg) {
        if (auto self = weak.lock()) self->onNotification(msg);
    });
    for (int t = 0; t < kTracks; ++t) loadTrack(m, t);
    loadKit(m);
}

void DeviceModel::setMachine(int track, int machine)
{
    if (machine >= 0 && machine <= 4 && machine != this->machine(track)) changeSetupByte(track, kSetupMachineOffset, machine);
}

void DeviceModel::setFilterMachine(int track, int filter)
{
    if (filter >= 0 && filter <= 5 && filter != filterMachine(track)) changeSetupByte(track, kSetupFilterOffset, filter);
}

void DeviceModel::changeSetupByte(int track, int offset, int value) { changeSetupBytes(track, {{offset, value}}); }

void DeviceModel::changeSetupBytes(int track, std::vector<std::pair<int, int>> bytes)
{
    if (!session_) return;
    std::weak_ptr<DeviceModel> weak = shared_from_this();
    std::lock_guard<std::mutex> l(session_->messengerLock());
    auto* m = session_->messenger();
    if (!m || !trackLoaded(track)) return;
    m->read(kReadSetup, (uint16_t)track, [weak, m, track, bytes](bool ok, const Message& msg) {
        // reader thread; m is the messenger running this callback
        if (!ok || msg.payload.size() != kSetupBytes) return;
        if ((msg.payload[0] << 8 | msg.payload[1]) != track) return;
        std::vector<uint8_t> setup = msg.payload;
        for (auto& [offset, value] : bytes) setup[(size_t)offset] = (uint8_t)value;
        m->write(0x10, kReadSetup, (uint16_t)track, std::move(setup));
        if (auto self = weak.lock()) self->pendingReload_ |= 1u << track;
    }, (uint32_t)track, 0);
}

void DeviceModel::reload() { pendingReload_ |= 0xffffu; }

int DeviceModel::lfoDestinationIndex(int lfo, int slot) { return lfoDestinationValue(lfo, slot); }

void DeviceModel::clearSound(int track)
{
    if (!session_ || track < 0 || track >= kTracks) return;
    {
        std::lock_guard<std::mutex> l(session_->messengerLock());
        auto* m = session_->messenger();
        if (!m) return;
        m->write(0x42, kReadSound, (uint16_t)track, {});
    }
    reloadAtMs_[(size_t)track] = nowMs() + 600;
}

void DeviceModel::writeSetup(int track, std::vector<uint8_t> setup)
{
    if (!session_ || setup.size() != kSetupBytes) return;
    {
        std::lock_guard<std::mutex> l(session_->messengerLock());
        auto* m = session_->messenger();
        if (!m) return;
        m->write(0x10, kReadSetup, (uint16_t)track, std::move(setup));
    }
    reloadAtMs_[(size_t)track] = nowMs() + 600;
}

int DeviceModel::modDestinationIndex(int slot) { return slot == 0 ? 0 : slot > 0 && slot < 107 && kSlotToMenu[slot] ? kSlotToMenu[slot] : -1; }

int DeviceModel::modValue(int track, int source, int entry, bool destination) const
{
    std::lock_guard<std::mutex> l(nameLock_);
    const auto& s = setups_[(size_t)track];
    if (s.size() != kSetupBytes || source < 0 || source > 5 || entry < 0 || entry > 3) return -1000;
    const size_t o = 0x35 + 12 * (size_t)source + 3 * (size_t)entry;
    return destination ? (int)s[o + 2] : (int)(int16_t)(s[o] << 8 | s[o + 1]);
}

void DeviceModel::setModValue(int track, int source, int entry, bool destination, int value)
{
    if (source < 0 || source > 5 || entry < 0 || entry > 3) return;
    const int o = 0x35 + 12 * source + 3 * entry;
    {
        std::lock_guard<std::mutex> l(nameLock_);
        auto& s = setups_[(size_t)track];
        if (s.size() != kSetupBytes) return;
        if (destination) s[(size_t)o + 2] = (uint8_t)value;
        else {
            s[(size_t)o] = (uint8_t)(value >> 8);
            s[(size_t)o + 1] = (uint8_t)value;
        }
    }
    if (destination) changeSetupBytes(track, {{o + 2, value & 0xff}});
    else changeSetupBytes(track, {{o, (value >> 8) & 0xff}, {o + 1, value & 0xff}});
}

int DeviceModel::lfoDestinationSlot(int lfo, int index)
{
    if (lfo < 0 || lfo > 2) return -1;
    for (int s = 0; s < 128; ++s)
        if (lfoDestinationValue(lfo, s) == index) return s;
    return -1;
}

// Value to send for a parameter so that the device stores what the container holds, -1 if none.
static int containerValue(const ParamDef& p, const std::vector<uint8_t>& c)
{
    if (p.slot >= 0 && p.slot < kSoundSlots) {
        const size_t o = (size_t)kSoundSlotOffset + 2 * (size_t)p.slot;
        int raw = c[o] << 8 | c[o + 1];
        for (int l = 0; l < 3; ++l)
            if (std::strcmp(p.key, l == 0 ? "Sound_Lfo1Destination" : l == 1 ? "Sound_Lfo2Destination" : "Sound_Lfo3Destination") == 0) {
                const int idx = lfoDestinationValue(l, c[o]);
                raw = idx < 0 ? -1 : idx << 8;
            }
        return raw;
    }
    for (auto& b : kByteParams)
        if (std::strcmp(p.key, b.key) == 0) return c[(size_t)b.offset] << 8;
    return -1;
}

void DeviceModel::applySound(int track, std::vector<uint8_t> c)
{
    if (!session_ || track < 0 || track >= kTracks || c.size() < (size_t)kSoundFilterOffset + 1 || c[0] != 0xBE || c[1] != 0xEF)
        return;
    auto self = shared_from_this();
    std::thread([self, track, c] { self->applyJob(track, c); }).detach();
}

// Writes a sound container into a track with the messages the device accepts (setup + automation):
// 1. slots only other machines / filter machines use (values left over from them): written while that
//    machine is active (the device keeps them when the machine changes back);
// 2. the setup (name, labels, machine, filter), then every parameter of the sound's machines.
void DeviceModel::applyJob(int track, std::vector<uint8_t> c)
{
    auto sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    std::vector<uint8_t> setup, now;
    for (int i = 0; i < 30; ++i) {
        {
            std::lock_guard<std::mutex> l(nameLock_);
            setup = setups_[(size_t)track];
            now = rawSounds_[(size_t)track];
        }
        if (setup.size() == kSetupBytes) break;
        sleep(100);
    }
    if (setup.size() != kSetupBytes) return;
    {
        // fresh copy of the track's sound: what the device holds right now
        auto got = std::make_shared<std::atomic<int>>(0);
        auto fresh = std::make_shared<std::vector<uint8_t>>();
        {
            std::lock_guard<std::mutex> l(session_->messengerLock());
            if (auto* m = session_->messenger())
                m->read(kReadSound, (uint16_t)track, [got, fresh](bool ok, const Message& msg) {
                    if (ok) *fresh = msg.payload;
                    *got = 1;
                }, (uint32_t)track, 0);
        }
        for (int i = 0; i < 40 && !*got; ++i) sleep(50);
        if (fresh->size() == now.size() || now.empty()) now = *fresh;
    }
    const int mach = c[kSoundMachineOffset], filt = c[kSoundFilterOffset];
    const auto& params = digitone2Params();
    auto activeFor = [&](const ParamDef& p, int m, int f) {
        return p.object == ParamObject::Sound && p.pid >= 0 && (machineOf(p) < 0 || machineOf(p) == m) &&
               (filterMachineOf(p) < 0 || filterMachineOf(p) == f);
    };
    // a slot is written through the sound's own parameter when that parameter can hold the value
    auto fits = [&](const ParamDef& p) {
        const int raw = containerValue(p, c);
        return raw >= p.min && raw <= p.max;
    };
    std::vector<bool> targetSlot(kSoundSlots, false);
    for (auto& p : params)
        if (activeFor(p, mach, filt) && p.slot >= 0 && p.slot < kSoundSlots && fits(p)) targetSlot[(size_t)p.slot] = true;

    // Leftovers of other machines (owner 0..4) and filter machines (owner 5..10). Activating a machine
    // resets its parameters, so each slot is written by the last activated owner that uses it.
    auto ownerOf = [](const ParamDef& p) { return machineOf(p) >= 0 ? machineOf(p) : filterMachineOf(p) >= 0 ? 5 + filterMachineOf(p) : -1; };
    std::map<int, std::vector<int>> candidates; // slot -> params of other owners that can hold the value
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        if (p.object != ParamObject::Sound || p.pid < 0 || p.slot < 0 || p.slot >= kSoundSlots) continue;
        if (targetSlot[(size_t)p.slot] || activeFor(p, mach, filt) || !fits(p) || ownerOf(p) < 0) continue;
        const size_t o = (size_t)kSoundSlotOffset + 2 * (size_t)p.slot;
        if (now.size() > o + 1 && now[o] == c[o] && now[o + 1] == c[o + 1]) continue; // already right
        candidates[p.slot].push_back((int)i);
    }
    std::vector<int> visits; // owners to activate, in order
    for (auto& [slot, list] : candidates) {
        const int o = ownerOf(params[(size_t)list.front()]);
        if (std::find(visits.begin(), visits.end(), o) == visits.end()) visits.push_back(o);
    }
    // activating an owner resets all its slots: those must be put back too
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        if (p.object != ParamObject::Sound || p.pid < 0 || p.slot < 0 || p.slot >= kSoundSlots) continue;
        if (targetSlot[(size_t)p.slot] || activeFor(p, mach, filt) || !fits(p)) continue;
        if (std::find(visits.begin(), visits.end(), ownerOf(p)) == visits.end()) continue;
        auto& list = candidates[p.slot];
        if (std::find(list.begin(), list.end(), (int)i) == list.end()) list.push_back((int)i);
    }
    std::map<int, std::vector<std::pair<int, int>>> foreign; // owner -> {param, raw}
    for (auto& [slot, list] : candidates) {
        int best = -1, bestPos = -1;
        for (int i : list) {
            const int pos = (int)(std::find(visits.begin(), visits.end(), ownerOf(params[(size_t)i])) - visits.begin());
            if (pos < (int)visits.size() && pos > bestPos) bestPos = pos, best = i;
        }
        if (best >= 0) foreign[visits[(size_t)bestPos]].push_back({best, containerValue(params[(size_t)best], c)});
    }

    auto writeSetup = [&](int m, int f, bool final) {
        std::vector<uint8_t> blob = setup;
        if (final) {
            std::memcpy(blob.data() + kSetupTagsOffset, c.data() + 8, 4);                // labels
            std::memcpy(blob.data() + kSetupNameOffset, c.data() + kSoundNameOffset, 16); // name
        }
        blob[kSetupMachineOffset] = (uint8_t)m;
        blob[kSetupFilterOffset] = (uint8_t)f;
        std::lock_guard<std::mutex> l(session_->messengerLock());
        if (auto* msg = session_->messenger()) msg->write(0x10, kReadSetup, (uint16_t)track, std::move(blob));
    };
    auto send = [&](const std::vector<std::pair<int, int>>& list) {
        std::lock_guard<std::mutex> l(session_->messengerLock());
        for (auto& [i, raw] : list) sendLocked(i, track, raw);
    };
    if (std::getenv("OB_APPLYDEBUG")) {
        for (int owner : visits)
            for (auto& [i, raw] : foreign[owner]) std::fprintf(stderr, "apply: owner %d %s slot %d raw %04x\n", owner, params[(size_t)i].key, params[(size_t)i].slot, raw);
    }
    for (int owner : visits) {
        const auto& list = foreign[owner];
        if (list.empty()) continue;
        if (owner < 5) writeSetup(owner, setup[kSetupFilterOffset], false);
        else writeSetup(mach, owner - 5, false);
        sleep(800);
        send(list);
        sleep(400);
    }
    writeSetup(mach, filt, true);
    sleep(800);
    std::vector<std::pair<int, int>> all;
    for (size_t i = 0; i < params.size(); ++i)
        if (activeFor(params[i], mach, filt) && fits(params[i]))
            if (const int raw = containerValue(params[i], c); raw >= 0) all.push_back({(int)i, raw});
    send(all);
    reloadAtMs_[(size_t)track] = nowMs() + 800;
}

void DeviceModel::loadTrack(Messenger* m, int track)
{
    std::weak_ptr<DeviceModel> weak = shared_from_this();
    m->read(kReadSound, (uint16_t)track, [weak, track](bool ok, const Message& msg) {
        if (!ok) return;
        if (auto self = weak.lock()) self->onSound(track, msg.payload);
    }, (uint32_t)track, 0); // the track goes in the argument and in the first payload word
    m->read(kReadSetup, (uint16_t)track, [weak, track](bool ok, const Message& msg) {
        if (!ok || msg.payload.size() != kSetupBytes || (msg.payload[0] << 8 | msg.payload[1]) != track) return;
        if (auto self = weak.lock()) {
            std::lock_guard<std::mutex> l(self->nameLock_);
            self->setups_[(size_t)track] = msg.payload;
        }
    }, (uint32_t)track, 0);
}

// Kit voice configuration (type 0x10) and kit setup (type 0x0F); stored by reply length (65 / 80 bytes).
void DeviceModel::loadKit(Messenger* m)
{
    std::weak_ptr<DeviceModel> weak = shared_from_this();
    auto store = [weak](bool ok, const Message& msg) {
        auto self = weak.lock();
        if (!ok || !self) return;
        std::lock_guard<std::mutex> l(self->nameLock_);
        if (msg.payload.size() == kVoiceConfigBytes) self->voiceConfig_ = msg.payload;
        else if (msg.payload.size() == kKitSetupBytes) self->kitSetup_ = msg.payload;
    };
    m->read(kitVoiceConfigType(session_->protocolVersion()), 0, store);
    m->read(kitSetupType(session_->protocolVersion()), 0, store);
    if (session_->protocolVersion() < 129) return; // protocol 127 does not answer these reads
    loadMixer(m);
}

namespace {
// Offsets of the send FX / master / compressor / Ext-In values (u16 BE raw) in the kit container
// (read 0x0E), by parameter pid. Measured on OS 1.12 by writing each parameter and reading the
// container back (tools/obsnapshot OB_MAPKIT). The Ext-In "Stereo" parameters share the storage
// of input 1.
struct FxOffset {
    int pid, offset;
};
constexpr FxOffset kKitFxOffsets[] = {
    {110, 0x16b2}, {111, 0x16b4}, {112, 0x16b6}, {113, 0x16b8}, {114, 0x16ba}, {115, 0x16bc}, {117, 0x16be},
    {119, 0x16c0}, {120, 0x16c2}, {121, 0x16c4}, {122, 0x16c6}, {123, 0x16c8}, {124, 0x16ca}, {125, 0x16cc},
    {127, 0x16ce}, {130, 0x16d2}, {131, 0x16d4}, {132, 0x16d6}, {133, 0x16d8}, {134, 0x16da}, {135, 0x16dc},
    {137, 0x16de}, {141, 0x16e2}, {142, 0x16e2}, {143, 0x16e4}, {144, 0x16e6}, {145, 0x16e6}, {146, 0x16e8},
    {147, 0x16ea}, {148, 0x16ea}, {149, 0x16ec}, {150, 0x16ee}, {151, 0x16ee}, {152, 0x16f0}, {153, 0x16f2},
    {154, 0x16f2}, {155, 0x16f4}, {156, 0x16f6}, {158, 0x16f8}, {160, 0x16fa}, {161, 0x16fc}, {162, 0x16fe},
    {163, 0x1700}, {164, 0x1702}, {165, 0x1704}, {167, 0x1706}, {168, 0x1708}, {169, 0x170a},
};
constexpr size_t kKitTrackLevels = 0x1c;    // 16 × u16 BE, one per track
constexpr size_t kKitContainerBytes = 10752; // version 4 (OS 1.12)
constexpr uint8_t kReadKit = 0x0e, kReadPatternMutes = 0x2e, kReadProjectSettings = 0x1a;
constexpr size_t kProjectSettingsMutes = 12; // u16 BE track mask (bit n = track n + 1)
} // namespace

void DeviceModel::loadMixer(Messenger* m)
{
    // Initial values of the mixer, send FX, master and compressor (kit container), plus the pattern mutes (0x2E, u16 mask) and the mixer mutes (project settings).
    // Solo is not stored by the device.
    std::weak_ptr<DeviceModel> weak = shared_from_this();
    m->read(kReadKit, 0, [weak](bool ok, const Message& msg) {
        auto self = weak.lock();
        const auto& b = msg.payload;
        if (!ok || !self || b.size() != kKitContainerBytes || b[0] != 0xbe || b[1] != 0xef || b[7] != 4) return;
        auto u16 = [&](size_t o) { return b[o] << 8 | b[o + 1]; };
        const auto& params = digitone2Params();
        for (size_t i = 0; i < params.size(); ++i) {
            const auto& p = params[i];
            if (p.object == ParamObject::Fx) {
                for (const auto& f : kKitFxOffsets)
                    if (f.pid == p.pid) self->deviceSet((int)i, 0, u16((size_t)f.offset));
            } else if (std::strcmp(p.key, "Kit_TrackLevel") == 0) {
                for (int t = 0; t < kTracks; ++t) self->deviceSet((int)i, t, u16(kKitTrackLevels + 2 * (size_t)t));
            }
        }
    });
    auto setMask = [weak](const char* key, int mask) {
        auto self = weak.lock();
        const auto* d = findParam(key);
        if (!self || !d) return;
        for (int t = 0; t < kTracks; ++t) self->deviceSet((int)(d - digitone2Params().data()), t, (mask >> t) & 1 ? d->max : d->min);
    };
    m->read(kReadPatternMutes, 0, [setMask](bool ok, const Message& msg) {
        if (ok && msg.payload.size() == 2) setMask("Kit_PatternTrackMute", msg.payload[0] << 8 | msg.payload[1]);
    });
    m->read(kReadProjectSettings, 0, [weak](bool ok, const Message& msg) {
        auto self = weak.lock();
        if (!ok || !self || msg.payload.size() < kProjectSettingsMutes + 2) return;
        const int mask = msg.payload[kProjectSettingsMutes] << 8 | msg.payload[kProjectSettingsMutes + 1];
        const auto* soloDef = findParam("Settings_TrackSolo");
        bool anySolo = false;
        for (int t = 0; soloDef && t < kTracks; ++t) anySolo = anySolo || self->get((int)(soloDef - digitone2Params().data()), t) > 0;
        {
            std::lock_guard<std::mutex> l(self->midiSenderLock_);
            for (int t = 0; t < kTracks; ++t) self->sentMute_[(size_t)t] = (mask >> t) & 1;
        }
        // While a solo is on, the device's mutes include the tracks the solo silences: their M buttons
        // keep their own state; the other tracks follow the device.
        const auto* muteDef = findParam("Settings_TrackMute");
        if (!muteDef) return;
        const int mute = (int)(muteDef - digitone2Params().data()), solo = (int)(soloDef - digitone2Params().data());
        for (int t = 0; t < kTracks; ++t) {
            if (anySolo && self->get(solo, t) == 0) continue;
            self->deviceSet(mute, t, (mask >> t) & 1 ? muteDef->max : muteDef->min);
        }
        if (anySolo) {
            // a solo restored with the host project: apply it to the device (the writer sends the
            // differences with the mutes just read)
            for (int t = 0; t < kTracks; ++t)
                if (self->get(solo, t) > 0) {
                    self->dirty_[(size_t)slotIndex(solo, t)].store(1, std::memory_order_release);
                    self->anyDirty_.store(true, std::memory_order_release);
                    break;
                }
        }
    });
}

void DeviceModel::writeKitObject(uint8_t type, std::vector<uint8_t> blob)
{
    if (!session_) return;
    std::lock_guard<std::mutex> l(session_->messengerLock());
    if (auto* m = session_->messenger()) m->write(0x10, type, 0, std::move(blob));
}

bool DeviceModel::voiceConfigLoaded() const
{
    std::lock_guard<std::mutex> l(nameLock_);
    return voiceConfig_.size() == kVoiceConfigBytes;
}

int DeviceModel::voiceSteal() const
{
    std::lock_guard<std::mutex> l(nameLock_);
    return voiceConfig_.size() == kVoiceConfigBytes ? voiceConfig_[0] : -1000;
}

int DeviceModel::voiceField(int track, Voice f) const
{
    std::lock_guard<std::mutex> l(nameLock_);
    if (voiceConfig_.size() != kVoiceConfigBytes || track < 0 || track >= kTracks) return -1000;
    const uint8_t* b = voiceConfig_.data() + 1 + 4 * track;
    switch (f) {
    case Voice::LockedVoices: return (int8_t)b[0];
    case Voice::Unison: return (b[1] & 0x80) == 0 ? 1 : 0;
    case Voice::VoiceAmount: return (b[1] & 0x7f) + 1;
    case Voice::Spread: return (int8_t)b[2];
    case Voice::Reuse: return (int8_t)b[3];
    }
    return -1000;
}

void DeviceModel::setVoiceSteal(int value)
{
    std::vector<uint8_t> blob;
    {
        std::lock_guard<std::mutex> l(nameLock_);
        if (voiceConfig_.size() != kVoiceConfigBytes) return;
        voiceConfig_[0] = (uint8_t)value;
        blob = voiceConfig_;
    }
    writeKitObject(kitVoiceConfigType(session_->protocolVersion()), std::move(blob));
}

void DeviceModel::setVoiceField(int track, Voice f, int value)
{
    std::vector<uint8_t> blob;
    {
        std::lock_guard<std::mutex> l(nameLock_);
        if (voiceConfig_.size() != kVoiceConfigBytes || track < 0 || track >= kTracks) return;
        uint8_t* b = voiceConfig_.data() + 1 + 4 * track;
        switch (f) {
        case Voice::LockedVoices: b[0] = (uint8_t)value; break;
        case Voice::Unison: b[1] = (uint8_t)((b[1] & 0x7f) | (value ? 0 : 0x80)); break;
        case Voice::VoiceAmount: b[1] = (uint8_t)((b[1] & 0x80) | ((value - 1) & 0x7f)); break;
        case Voice::Spread: b[2] = (uint8_t)value; break;
        case Voice::Reuse: b[3] = (uint8_t)value; break;
        }
        blob = voiceConfig_;
    }
    writeKitObject(kitVoiceConfigType(session_->protocolVersion()), std::move(blob));
}

bool DeviceModel::kitSetupLoaded() const
{
    std::lock_guard<std::mutex> l(nameLock_);
    return kitSetup_.size() == kKitSetupBytes;
}

int DeviceModel::trigMask(int track) const
{
    std::lock_guard<std::mutex> l(nameLock_);
    if (kitSetup_.size() != kKitSetupBytes || track < 0 || track >= kTracks) return -1000;
    return kitSetup_[0x10 + 2 * (size_t)track] << 8 | kitSetup_[0x11 + 2 * (size_t)track];
}

void DeviceModel::setTrigMask(int track, int mask)
{
    std::vector<uint8_t> blob;
    {
        std::lock_guard<std::mutex> l(nameLock_);
        if (kitSetup_.size() != kKitSetupBytes || track < 0 || track >= kTracks) return;
        kitSetup_[0x10 + 2 * (size_t)track] = (uint8_t)(mask >> 8);
        kitSetup_[0x11 + 2 * (size_t)track] = (uint8_t)mask;
        blob = kitSetup_;
    }
    writeKitObject(kitSetupType(session_->protocolVersion()), std::move(blob));
}

int DeviceModel::setupField(int track, SetupField f) const
{
    std::lock_guard<std::mutex> l(nameLock_);
    const auto& s = setups_[(size_t)track];
    if (s.size() != kSetupBytes) return -1000;
    return f == Transpose ? (int)(int8_t)s[(size_t)f] : (int)s[(size_t)f];
}

void DeviceModel::setSetupField(int track, SetupField f, int value)
{
    {
        std::lock_guard<std::mutex> l(nameLock_);
        auto& s = setups_[(size_t)track];
        if (s.size() == kSetupBytes) s[(size_t)f] = (uint8_t)value;
    }
    changeSetupByte(track, (int)f, value & 0xff);
}

void DeviceModel::onSound(int track, const std::vector<uint8_t>& b)
{
    if (b.size() < (size_t)kSoundFilterOffset + 1 || b[0] != 0xBE || b[1] != 0xEF || b[2] != 0xBA || b[3] != 0xCE) return;
    const int mach = b[kSoundMachineOffset];
    const int filt = b[kSoundFilterOffset];
    machine_[(size_t)track] = mach;
    tags_[(size_t)track] = (uint32_t)b[8] << 24 | (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11];
    filter_[(size_t)track] = filt;
    {
        std::string name; // latin-1 on the device (Å, Ä, Ö...) -> UTF-8
        for (int i = 0; i < 16 && b[(size_t)kSoundNameOffset + (size_t)i]; ++i) {
            const uint8_t c = b[(size_t)kSoundNameOffset + (size_t)i];
            if (c < 0x80) name += (char)c;
            else {
                name += (char)(0xC0 | (c >> 6));
                name += (char)(0x80 | (c & 0x3F));
            }
        }
        std::lock_guard<std::mutex> l(nameLock_);
        names_[(size_t)track] = name;
    }
    const auto& params = digitone2Params();
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        if (p.object != ParamObject::Sound || p.slot < 0 || p.slot >= kSoundSlots) continue;
        if (machineOf(p) >= 0 && machineOf(p) != mach) continue;
        if (filterMachineOf(p) >= 0 && filterMachineOf(p) != filt) continue;
        const size_t o = (size_t)kSoundSlotOffset + 2 * (size_t)p.slot;
        deviceSet((int)i, track, b[o] << 8 | b[o + 1]);
    }
    loaded_[(size_t)track] = true;
    {
        std::lock_guard<std::mutex> l(nameLock_);
        rawSounds_[(size_t)track] = b;
    }
    lastLoadMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool DeviceModel::inSyncWindow() const
{
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return lastLoadMs_.load() == 0 || now - lastLoadMs_.load() < 5000;
}

void DeviceModel::onNotification(const Message& m)
{
    const uint8_t type = m.header.type;
    static const bool logAll = std::getenv("OB_LOGNOTIFY") != nullptr;
    if (logAll) {
        std::fprintf(stderr, "notify cls %02x type %02x arg %u:", m.header.cls, type, m.header.arg);
        for (size_t i = 0; i < m.payload.size() && i < 24; ++i) std::fprintf(stderr, " %02x", m.payload[i]);
        std::fprintf(stderr, "\n");
    }
    if (m.payload.size() >= 6 && (type == kObjSound || type == kObjFx || type == kObjKit || type == kObjKitPerTrack ||
                                  type == kObjSettings)) {
        const int a = m.payload[0] << 8 | m.payload[1];
        const int pid = m.payload[2] << 8 | m.payload[3];
        const int value = m.payload[4] << 8 | m.payload[5];
        const ParamObject obj = type == kObjSound ? ParamObject::Sound
                              : type == kObjFx    ? ParamObject::Fx
                              : type == kObjSettings ? ParamObject::Settings
                                                     : ParamObject::Kit;
        const auto& list = byPid_[(int)obj];
        if (pid < 0 || pid >= (int)list.size()) return;
        const int track = obj == ParamObject::Fx ? 0 : a & 0x0f;
        for (int idx : list[(size_t)pid]) {
            const auto& p = digitone2Params()[(size_t)idx];
            if (machineOf(p) >= 0 && machineOf(p) != machine(track)) continue;
            if (filterMachineOf(p) >= 0 && filterMachineOf(p) != filterMachine(track)) continue;
            deviceSet(idx, track, value);
        }
        return;
    }
    // Sound setup / kit changed on the device: reload (machine, names, all values).
    if ((type == 4 || type == 6) && m.payload.size() >= 2) { // a track's sound changed (machine, load ...)
        pendingReload_ |= 1u << ((m.payload[0] << 8 | m.payload[1]) & 0x0f);
        return;
    }
    if (type == 4 || type == 6 || type == 8 || type == 10 || type == 0x0e || type == 0x10 || type == 0x11 || type == 0x1a) {
        if (type == 6 && m.payload.size() >= 2) pendingReload_ |= 1u << ((m.payload[0] << 8 | m.payload[1]) & 0x0f);
        else reload();
        return;
    }
    std::fprintf(stderr, "OpenBridge: unhandled device message class 0x%02x type 0x%02x arg %u (%zu bytes)\n",
                 m.header.cls, type, m.header.arg, m.payload.size());
}

void DeviceModel::deviceSet(int param, int track, int raw)
{
    auto& slot = values_[(size_t)slotIndex(param, track)];
    if (slot.exchange(raw) == raw) return;
    std::lock_guard<std::mutex> l(changeLock_);
    changes_.push_back({param, track, raw});
}

std::vector<DeviceModel::Change> DeviceModel::takeChanges()
{
    std::lock_guard<std::mutex> l(changeLock_);
    std::vector<Change> c;
    c.swap(changes_);
    return c;
}

int DeviceModel::get(int param, int track) const { return values_[(size_t)slotIndex(param, track)].load(); }

bool DeviceModel::writable(int param, int track) const
{
    const auto& p = digitone2Params()[(size_t)param];
    // Nothing is sent before the device state is known: the host restores parameter values
    // when a project loads, and those must not overwrite the device.
    if (p.object == ParamObject::Fx ? !trackLoaded(0) : !trackLoaded(track)) return false;
    if (machineOf(p) >= 0) return trackLoaded(track) && machineOf(p) == machine(track);
    if (filterMachineOf(p) >= 0) return trackLoaded(track) && filterMachineOf(p) == filterMachine(track);
    return true;
}

void DeviceModel::set(int param, int track, int raw)
{
    const auto& p = digitone2Params()[(size_t)param];
    raw = std::max(p.min, std::min(p.max, raw));
    const size_t i = (size_t)slotIndex(param, track);
    if (values_[i].exchange(raw) == raw) return; // unchanged: nothing to send
    if (!writable(param, track)) return;
    dirty_[i].store(1, std::memory_order_release);
    anyDirty_.store(true, std::memory_order_release);
    // no notify here: the writer polls every few milliseconds (notify may lock on some platforms)
}

void DeviceModel::writerLoop()
{
    const auto& params = digitone2Params();
    while (!quit_.load()) {
        {
            std::unique_lock<std::mutex> l(wakeLock_);
            wake_.wait_for(l, std::chrono::milliseconds(4));
        }
        if (!anyDirty_.exchange(false) || !session_) continue;
        std::lock_guard<std::mutex> l(session_->messengerLock());
        for (size_t p = 0; p < params.size(); ++p)
            for (int t = 0; t < kTracks; ++t) {
                const size_t i = (size_t)slotIndex((int)p, t);
                if (params[p].object == ParamObject::Fx && t > 0) break;
                if (dirty_[i].exchange(0, std::memory_order_acq_rel)) sendLocked((int)p, t, values_[i].load());
            }
    }
}

bool DeviceModel::sendLocked(int param, int track, int raw)
{
    auto* m = session_->messenger();
    if (!m) return false;
    const auto& p = digitone2Params()[(size_t)param];
    switch (p.object) {
    case ParamObject::Sound: m->setParam(kObjSound, (uint16_t)track, (uint16_t)track, (uint16_t)p.pid, (uint16_t)raw); break;
    case ParamObject::Fx: m->setParam(kObjFx, 0x10, 0x10, (uint16_t)p.pid, (uint16_t)raw); break;
    case ParamObject::Settings: {
        // Mute and solo of the mixer. The device keeps track mutes (project settings) but no solo
        // (the device does not keep a solo state), so solo is done
        // as a mixer does: a track is muted when its M is on, or when another track is soloed.
        std::lock_guard<std::mutex> l(midiSenderLock_);
        sendTrackMutesLocked(m);
        break;
    }
    case ParamObject::Kit:
        if (session_->protocolVersion() >= 129) {
            // Kit parameters as object 0x0F, the 16 pattern mutes as
            // 0x2D; arg 0, a = track. Verified on OS 1.12 (track level, pattern mutes read back);
            // OS 1.10D ignores these objects.
            const uint8_t obj = isPatternMute(p) ? kObjKitPerTrack : kObjKit;
            m->setParam(obj, 0, (uint16_t)track, (uint16_t)p.pid, (uint16_t)raw);
            break;
        }
        {
            // Older OS (protocol 127, OS 1.10D): the parameters' MIDI CCs on the track's channel
            // (CC 95 track level, 110 pattern mute).
            std::lock_guard<std::mutex> l(midiSenderLock_);
            if (midiSender_ && p.cc >= 0)
                midiSender_(track + 1, p.cc, p.max <= 256 ? (raw > p.min ? 127 : 0) : std::min(127, raw >> 8));
        }
        break;
    }
    return true;
}

void DeviceModel::sendTrackMutesLocked(Messenger* m)
{
    const auto* muteDef = findParam("Settings_TrackMute");
    const auto* soloDef = findParam("Settings_TrackSolo");
    if (!muteDef || !soloDef) return;
    const int mute = (int)(muteDef - digitone2Params().data()), solo = (int)(soloDef - digitone2Params().data());
    const bool usb = session_->protocolVersion() >= 129;
    if (!usb && !midiSender_) return;
    bool anySolo = false;
    for (int t = 0; t < kTracks; ++t) anySolo = anySolo || get(solo, t) > 0;
    for (int t = 0; t < kTracks; ++t) {
        const int muted = get(mute, t) > 0 || (anySolo && get(solo, t) == 0) ? 1 : 0;
        if (sentMute_[(size_t)t] == muted) continue;
        sentMute_[(size_t)t] = muted;
        // OS 1.12: mute message (object 0x1B, a = track, pid of Mute); it sets the
        // same project-settings mute bit as CC 94. OS 1.10D: CC 94 on the track's channel.
        if (usb) m->setParam(kObjSettings, 0, (uint16_t)t, (uint16_t)muteDef->pid, (uint16_t)(muted ? muteDef->max : muteDef->min));
        else midiSender_(t + 1, muteDef->cc, muted ? 127 : 0);
    }
}

} // namespace ob
