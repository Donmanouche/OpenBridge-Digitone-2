// The plugin's copy of the Digitone II state: parameter values per track, active machines,
// sound names. Loaded from the device (sound containers), kept up to date by the device's
// notifications, and written back with automation messages.
#pragma once

#include "DeviceSession.h"
#include "core/Digitone2Params.h"

#include <array>
#include <condition_variable>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ob {

class DeviceModel : public std::enable_shared_from_this<DeviceModel> {
public:
    static constexpr int kTracks = 16;

    // Sends a MIDI control change to the device's USB MIDI port (channel 1..16). Kit and settings
    // parameters (track level, mutes, solo) go this way: the device ignores them as USB automation.
    void setMidiSender(std::function<void(int channel, int cc, int value)> fn)
    {
        std::lock_guard<std::mutex> l(midiSenderLock_);
        midiSender_ = std::move(fn);
    }

    struct Change {
        int param; // index in digitone2Params()
        int track;
        int raw;
    };

    static std::shared_ptr<DeviceModel> create(std::shared_ptr<DeviceSession> session);
    ~DeviceModel();

    // Call regularly from the message thread: (re)attaches after a connection and reloads the state.
    void poll();
    void setSession(std::shared_ptr<DeviceSession> session);

    bool connected() const;
    bool trackLoaded(int track) const { return loaded_[(size_t)track].load(); }
    int machine(int track) const
    {
        const int d = displayMachine_[(size_t)track].load();
        return d >= 0 ? d : machine_[(size_t)track].load();
    }
    int filterMachine(int track) const { return filter_[(size_t)track].load(); }
    std::string soundName(int track) const;
    uint32_t soundTags(int track) const { return tags_[(size_t)track].load(); } // preset labels, bit 0 = Kick

    int get(int param, int track) const;
    // From the editor or the host: stores the value and marks it for sending. Lock-free and
    // allocation-free: safe on the audio thread. A writer thread sends the marked values.
    // Values equal to the current one are ignored, and nothing is sent for a track whose
    // state has not been read from the device yet.
    void set(int param, int track, int raw);
    // Stores a value without sending it (host state restore).
    void store(int param, int track, int raw) { values_[(size_t)slotIndex(param, track)].store(raw); }
    // True for a few seconds after the device state was (re)loaded: host writes from its
    // message thread in that window are project restores, not edits.
    bool inSyncWindow() const;
    // True when a write of this parameter on this track reaches the device right now
    // (machine parameters only reach the device when their machine is active).
    bool writable(int param, int track) const;

    // Values changed by the device since the last call (message thread).
    std::vector<Change> takeChanges();

    // Changes the machine (0..4) / filter machine (0..5) of a track: reads the sound setup,
    // changes that byte and writes it back (class 0x10 type 6). The device resets the machine's
    // parameters, as when the machine is changed on the device.
    void setMachine(int track, int machine);
    void setFilterMachine(int track, int filter);

    // Loads a sound container (e.g. from a +Drive preset file, read over SysEx) into a track,
    // with the writes the device accepts: the sound setup (name, labels, machine, filter; class
    // 0x10 type 6), then, once the device has switched machines, every parameter of the sound.
    // (The device ignores the storage operations class 0x43 / 0x44.)
    void applySound(int track, std::vector<uint8_t> container);

    // Sound setup fields (read / write type 6, 125 bytes). -1000 when not read yet.
    enum SetupField {
        VelocityToVolume = 24, // 0 Off, 1 Logarithm, 2 Linear, 3 Exponential, 4 Exponential 2
        PitchBendDepth = 28,   // 0..60
        Transpose = 29,        // signed, -60..60 semitones ("Octave offset")
        PlayMode = 30,         // 0 Poly, 1 Poly (M. LFO), 2 Mono, 3 Mono (Leg)
        MonoNotePriority = 31, // 0 Last, 1 Low, 2 High
        PortamentoAmount = 32,
        PortamentoType = 33,   // 0 Track, 1 Voice, 2 Legato Only
        PortamentoStyle = 34,  // 0 Glide, 1 Glissando
        PortamentoSlope = 35,  // 0 Constant rate, 1 Constant time
        PortamentoGating = 36,
    };
    int setupField(int track, SetupField f) const;
    void setSetupField(int track, SetupField f, int value);

    // Kit voice configuration (65 bytes: voice stealing, then per track locked voices, unison (bit 7
    // clear) + voice amount - 1, spread, reuse) and kit setup (80 bytes: name[16], track trig masks
    // u16 BE [16] @0x10). Object types depend on the protocol version (DeviceModel.cpp).
    enum class Voice { LockedVoices, Unison, VoiceAmount, Spread, Reuse };
    bool voiceConfigLoaded() const;
    int voiceSteal() const;                  // 0 Cycle, 1 Track, 2 Low, 3 High (global)
    int voiceField(int track, Voice f) const; // -1000 when not read
    void setVoiceSteal(int value);
    void setVoiceField(int track, Voice f, int value);
    bool kitSetupLoaded() const;
    int trigMask(int track) const;           // bit t: track t + 1 is triggered with this track (LAYER)
    void setTrigMask(int track, int mask);

    // PRESET "Clear": the device resets the track's sound (header-only class 0x42, type 4, arg track).
    void clearSound(int track);
    // Writes a whole sound setup (125 bytes, read type 6) back.
    void writeSetup(int track, std::vector<uint8_t> setup);

    // Snapshots only: show a machine without telling the device (tools/obsnapshot OB_SHOWMACHINE).
    void displayMachine(int track, int machine) { displayMachine_[(size_t)track] = machine; }

    // Reload every track from the device.
    void reload();

    // LFO destination menu (automation value >> 8) <-> slot of the target in the sound container
    // (measured on the device). -1 when unknown.
    static int lfoDestinationSlot(int lfo, int index);
    static int lfoDestinationIndex(int lfo, int slot);
    // Modulation sources (sound setup @0x35: KEY TR., PITCH BEND, VELOCITY, MOD WHEEL, BREATH,
    // AFTERTOUCH; 4 entries of {depth i16 BE, destination slot u8}).
    static int modDestinationIndex(int slot); // menu position of a destination slot, -1 if none
    int modValue(int track, int source, int entry, bool destination) const; // -1000 when not read
    void setModValue(int track, int source, int entry, bool destination, int value);

    static int slotIndex(int param, int track)
    {
        return digitone2Params()[(size_t)param].object == ParamObject::Fx ? param * kTracks : param * kTracks + track;
    }

private:
    explicit DeviceModel(std::shared_ptr<DeviceSession> session);
    void attach(Messenger* m);
    void loadTrack(Messenger* m, int track);
    void changeSetupByte(int track, int offset, int value);
    void changeSetupBytes(int track, std::vector<std::pair<int, int>> bytes); // {offset, value}
    void onSound(int track, const std::vector<uint8_t>& blob);
    void onNotification(const Message& m);
    void deviceSet(int param, int track, int raw);
    bool sendLocked(int param, int track, int raw);
    std::mutex midiSenderLock_;
    std::function<void(int, int, int)> midiSender_;
    std::array<int, 16> sentMute_{}; // last mute sent per track (tracks start unmuted)
    void releaseSoloLocked(Messenger* m);
    void sendTrackMutesLocked(Messenger* m); // mute + solo -> the device's track mutes

    std::shared_ptr<DeviceSession> session_;
    int generation_ = -1;

    std::vector<std::atomic<int>> values_;
    std::array<std::atomic<int>, kTracks> machine_{};
    std::array<std::atomic<int>, kTracks> filter_{};
    std::array<std::atomic<int>, kTracks> displayMachine_{{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}};
    std::array<std::atomic<bool>, kTracks> loaded_{};
    std::array<std::atomic<uint32_t>, kTracks> tags_{};
    mutable std::mutex nameLock_;
    std::array<std::string, kTracks> names_;
    std::array<std::vector<uint8_t>, kTracks> setups_; // guarded by nameLock_
    std::vector<uint8_t> voiceConfig_, kitSetup_;    // guarded by nameLock_
    void loadKit(Messenger* m);
    void loadMixer(Messenger* m);
    void writeKitObject(uint8_t type, std::vector<uint8_t> blob);

    void writerLoop();
    std::vector<std::atomic<uint8_t>> dirty_;
    std::atomic<bool> anyDirty_{false};
    std::thread writer_;
    std::atomic<bool> quit_{false};
    std::mutex wakeLock_;
    std::condition_variable wake_;

    std::atomic<int64_t> lastLoadMs_{0};
    std::atomic<uint32_t> pendingReload_{0}; // bit per track, set by notifications
    std::array<std::atomic<int64_t>, kTracks> reloadAtMs_{};  // delayed reload after a storage operation
    void applyJob(int track, std::vector<uint8_t> container);
    std::array<std::vector<uint8_t>, kTracks> rawSounds_; // last sound container read, guarded by nameLock_
    std::mutex changeLock_;
    std::vector<Change> changes_;
    std::vector<std::vector<int>> byPid_[4]; // object -> pid -> params
};

} // namespace ob
