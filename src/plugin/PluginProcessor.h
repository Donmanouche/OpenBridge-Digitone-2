#pragma once

#include "DeviceModel.h"
#include "DeviceSession.h"
#include "core/DriftResampler.h"
#include "core/PresetLibrary.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>

class OpenBridgeProcessor;

// One device parameter on one track, exposed to the host.
class DeviceParameter : public juce::AudioProcessorParameterWithID {
public:
    DeviceParameter(OpenBridgeProcessor& p, int param, int track, const juce::String& id, const juce::String& name);

    float getValue() const override;
    void setValue(float v) override;
    float getDefaultValue() const override;
    juce::String getText(float v, int maximumLength) const override;
    float getValueForText(const juce::String& text) const override;
    int getNumSteps() const override;
    bool isDiscrete() const override { return steps_ > 0 && steps_ <= 128; }
    juce::String getLabel() const override { return {}; }

    // Called on the message thread when the device changed the value.
    void updateFromDevice(int raw);

    const ob::ParamDef& def() const;
    const int param, track;

private:
    OpenBridgeProcessor& proc_;
    int steps_;
    std::atomic<float> value_;
    bool fromDevice_ = false;
};

class OpenBridgeProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    OpenBridgeProcessor();
    ~OpenBridgeProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OpenBridge Digitone II"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // --- editor access
    ob::DeviceSession* session() { return session_.get(); }
    ob::DeviceModel& model() { return *model_; }
    std::shared_ptr<ob::DeviceModel> modelPtr() { return model_; }
    const ob::DeviceDesc& desc() const { return *desc_; }
    void selectDevice(const std::string& id);
    juce::String selectedDeviceId() const { return wantedId_; }
    bool isAudioOwner() const { return owner_; }
    // Host parameter of (param, track); Fx parameters use track 0. nullptr if not exposed.
    DeviceParameter* parameterFor(int param, int track);
    // Sends a MIDI message to the device (virtual keyboard).
    void sendMidiToDevice(const juce::MidiMessage& m);
    bool midiOutputOpen() const { return midiOut_ != nullptr; }
    // Test helper: enables every bus (hosts do this through the bus layout API).
    void enableAllBuses()
    {
        for (int i = 0; i < getBusCount(false); ++i) getBus(false, i)->enable();
        for (int i = 0; i < getBusCount(true); ++i) getBus(true, i)->enable();
    }
    // +Drive preset library, read over SysEx; started on first use, shared by the plugin instances.
    std::shared_ptr<ob::PresetLibrary> presetLibrary();
    // Level meters: peak of a device output channel (0 = Main L, 2 + 2t = Track t + 1 L, ...) since the
    // last call. 0 when the audio is not running.
    float takeChannelPeak(int channel)
    {
        return channel >= 0 && channel < (int)channelPeak_.size() ? channelPeak_[(size_t)channel].exchange(0.0f) : 0.0f;
    }
    int outputChannelIndex(const std::string& name) const; // first channel of an output ("Main", "Track 3", "Delay")
    // Set while the editor changes a parameter, so the change is sent even in the sync window.
    std::atomic<bool> editorGesture{false};
    float uiZoom = 1.0f; // 100 %
    int uiPage = 1;  // 0 kit, 1..16 tracks, 17 send fx, 18 mixer
    int uiLfo = 0;
    int uiSetupTab = 0;
    bool uiKeyboard = false; // on-screen keyboard shown // SETUP panel: TRK, VOICE, PORTAMENTO, LAYER
    bool uiPresetBrowser = false; // editor shows the Preset Browser instead of the Kit Editor
    int uiActiveTrack = 0;  // track the Preset Browser loads into (0..15)
    int uiLibraryBank = 0;  // bank shown by the Preset Browser (0..7 = A..H)
    uint32_t uiTagFilter = 0;
    bool uiHideFilters = false;

private:
    static BusesProperties makeBuses();
    void connectSession();
    void timerCallback() override;
    void openMidiOutput();

    const ob::DeviceDesc* desc_;
    std::vector<ob::StereoBus> outBuses_, inBuses_;
    std::shared_ptr<ob::DeviceSession> session_;
    std::shared_ptr<ob::DeviceModel> model_;
    std::shared_ptr<ob::PresetLibrary> library_;
    std::array<std::atomic<float>, 64> channelPeak_{};
    juce::String wantedId_;
    bool owner_ = false;
    std::vector<DeviceParameter*> byIndex_; // slotIndex -> parameter

    // MIDI to the device: the audio thread pushes into a lock-free FIFO, a thread sends.
    struct MidiEvent {
        juce::uint8 data[3];
        int size;
    };
    void midiLoop();
    std::unique_ptr<juce::MidiOutput> midiOut_;
    juce::CriticalSection midiLock_;
    int midiRetry_ = 0;
    juce::AbstractFifo midiFifo_{1024};
    std::array<MidiEvent, 1024> midiEvents_{};
    std::thread midiThread_;
    std::atomic<bool> midiQuit_{false};

    ob::DriftResampler fromDevice_, toDevice_;
    std::vector<float> interleavedOut_, interleavedIn_;
    double hostRate_ = 48000;
    int maxBlock_ = 512;
    bool prepared_ = false;
    std::atomic<int> pendingLatency_{-1};
    // diagnostics (~/.local/share/OpenBridge/openbridge.log)
    std::atomic<uint64_t> processCalls_{0};
    std::atomic<float> worstProcessMs_{0};
    std::atomic<int> lastBlock_{0};
    std::atomic<float> mainPeak_{0};
    int logTicks_ = 0;
    juce::String lastStatus_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenBridgeProcessor)
};
