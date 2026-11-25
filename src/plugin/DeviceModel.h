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
    // True when the device runs the alternative firmware with the Digitakt II machines (One Shot ...).
    bool altMachinesAvailable() const;

    // Total Recall: last session state the device reported (notification 0x2B: 1 disabled, 2 active,
    // 3 aborted, 8 choose device or plugin, 9 send the project, 10 the device is master ...), how many
    // state notifications arrived, and how many session id changes (notification 0x2C).
    int totalRecallState() const { return trState_.load(); }
    int totalRecallStateCount() const { return trStateCount_.load(); }
    int totalRecallSessionChanges() const { return trSessionChanges_.load(); }

    // ---- Total Recall: the device's whole project kept
    // in the host project. trState() is the session state the device reports (notification 0x2B):
    // 0 undetermined, 1 disabled, 2 active, 3 aborted, 4..6 new session, 7..10 recall session
    // (8: waiting for the user's choice), 11/12 transfer host -> device / device -> host.
    int trState() const { return trState_.load(); }
    bool trEnabled() const;            // a session id and a project image are stored
    bool trAutoSync() const { return trAutoSync_.load(); }
    void trSetAutoSync(bool on) { trAutoSync_ = on; }
    int64_t trTimestamp() const;       // last sync, ms since the epoch (0: never)
    std::string trStatusText() const;  // message for trState()
    void trEnableOrSync();             // ENABLE NOW (new session, the device's state kept) / SYNC NOW
    void trDisable();
    void trReactivate();               // recall session: the device and the plugin offer the choice again
    enum class TrChoice { Cancel, PluginState, DeviceState };
    void trChoose(TrChoice c);         // answer to "Which state would you like to use?" (trState() == 8)
    struct TrSaved {
        uint32_t sessionId = 0;        // 0: Total Recall disabled
        std::vector<uint8_t> image;    // LZ4 frame of the project as the device sends it (type 0x1D)
        int64_t timestamp = 0;
        bool autoSync = true;
    };
    void trSetSaved(TrSaved s);        // host project restored: starts a recall session
    TrSaved trGetSaved();              // host saves: with auto-sync, re-reads the project first (10 s max)
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
    void subscribe(Messenger* m); // device -> host parameter notifications
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
    std::atomic<bool> pendingMixer_{false};
    std::atomic<int> trState_{-1}, trStateCount_{0}, trSessionChanges_{0};
    // Total Recall session (see pollTotalRecall)
    void pollTotalRecall(Messenger* m);
    void trHandleState(Messenger* m, int state);
    void trRequest(Messenger* m, int mode, uint32_t id);
    void trReadProject(Messenger* m, std::function<void(bool)> done = {});
    mutable std::mutex trLock_;
    std::vector<uint8_t> trImage_;
    uint32_t trSessionId_ = 0;
    int64_t trTimestamp_ = 0;
    std::vector<int> trStates_;               // states from notifications, handled by poll (trLock_)
    std::atomic<bool> trWork_{false};         // states, actions or a recall to handle
    std::atomic<bool> trAutoSync_{true};
    std::atomic<int> trPending_{0};           // 9 / 10: the choice, applied when the device accepts (state 6)
    std::atomic<int> trActions_{0};           // editor requests (kTr... bits), executed by poll
    std::atomic<bool> trInitiate_{false};     // start a recall session once the device is ready
    std::atomic<bool> trReading_{false};
    int trGeneration_ = -1;   // project settings changed: read the mixer state again
    std::array<std::atomic<int64_t>, kTracks> reloadAtMs_{};  // delayed reload after a storage operation
    void applyJob(int track, std::vector<uint8_t> container);
    std::array<std::vector<uint8_t>, kTracks> rawSounds_; // last sound container read, guarded by nameLock_
    std::mutex changeLock_;
    std::vector<Change> changes_;
    std::vector<std::vector<int>> byPid_[4]; // object -> pid -> params
};

} // namespace ob
