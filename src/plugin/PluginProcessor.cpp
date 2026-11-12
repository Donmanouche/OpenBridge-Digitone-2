#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <chrono>
#include <cstring>
#include <mutex>

namespace {
constexpr uint16_t kDigitone2Pid = 0x0b34;

void logLine(const juce::String& s)
{
    static juce::CriticalSection lock;
    const juce::ScopedLock l(lock);
    auto f = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".local/share/OpenBridge/openbridge.log");
    f.getParentDirectory().createDirectory();
    if (f.getSize() > 2 * 1024 * 1024) f.deleteFile();
    f.appendText(juce::Time::getCurrentTime().toString(true, true, true, true) + "  " + s + "\n");
}
const ob::DeviceDesc& digitone2() { return *ob::findDeviceDesc(ob::kElektronVid, kDigitone2Pid); }
} // namespace

juce::AudioProcessor::BusesProperties OpenBridgeProcessor::makeBuses()
{
    BusesProperties p;
    for (auto& b : ob::groupChannels(digitone2().toDevice))
        p.addBus(true, b.name, b.channels == 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono(),
                 b.name == "Main");
    for (auto& b : ob::groupChannels(digitone2().toHost))
        p.addBus(false, b.name, b.channels == 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono(), true);
    return p;
}

// ------------------------------------------------------------------------------------------ parameters
DeviceParameter::DeviceParameter(OpenBridgeProcessor& p, int prm, int trk, const juce::String& id, const juce::String& name)
    : AudioProcessorParameterWithID(juce::ParameterID{id, 1}, name), param(prm), track(trk), proc_(p)
{
    const auto& d = def();
    const bool fine = d.decimals || d.format == ob::ParamFormat::Decimal || d.format == ob::ParamFormat::BipolarDecimal;
    steps_ = fine ? 0 : (d.max - d.min) / 256 + 1;
    value_ = ob::normalisedFromRaw(d, d.def);
}

const ob::ParamDef& DeviceParameter::def() const { return ob::digitone2Params()[(size_t)param]; }

float DeviceParameter::getValue() const { return value_.load(); }

void DeviceParameter::setValue(float v)
{
    value_ = v;
    if (fromDevice_) return;
    const int raw = ob::rawFromNormalised(def(), v);
    // Host writes on its message thread right after the device state was loaded are project
    // restores: keep them local. Automation (audio thread) and editor edits are sent.
    if (!proc_.editorGesture.load() && proc_.model().inSyncWindow() &&
        juce::MessageManager::existsAndIsCurrentThread())
        proc_.model().store(param, track, raw);
    else
        proc_.model().set(param, track, raw);
}

void DeviceParameter::updateFromDevice(int raw)
{
    fromDevice_ = true;
    setValueNotifyingHost(ob::normalisedFromRaw(def(), raw));
    fromDevice_ = false;
}

float DeviceParameter::getDefaultValue() const { return ob::normalisedFromRaw(def(), def().def); }

juce::String DeviceParameter::getText(float v, int) const { return ob::formatValue(def(), ob::rawFromNormalised(def(), v)); }

float DeviceParameter::getValueForText(const juce::String& text) const
{
    const auto& d = def();
    double x = text.retainCharacters("-0123456789.").getDoubleValue();
    if (d.format == ob::ParamFormat::Bipolar || d.format == ob::ParamFormat::BipolarDecimal || d.format == ob::ParamFormat::Pan)
        x += 64.0;
    if (d.format == ob::ParamFormat::OnOff) x = text.trim().equalsIgnoreCase("on") ? 1 : 0;
    return ob::normalisedFromRaw(d, (int)std::lround(x * 256.0));
}

int DeviceParameter::getNumSteps() const { return steps_ > 0 ? steps_ : juce::AudioProcessor::getDefaultNumParameterSteps(); }

// ------------------------------------------------------------------------------------------ processor
OpenBridgeProcessor::OpenBridgeProcessor() : AudioProcessor(makeBuses()), desc_(&digitone2())
{
    outBuses_ = ob::groupChannels(desc_->toHost);
    inBuses_ = ob::groupChannels(desc_->toDevice);
    connectSession();
    model_ = ob::DeviceModel::create(session_);
    model_->setMidiSender([this](int channel, int cc, int value) {
        sendMidiToDevice(juce::MidiMessage::controllerEvent(channel, cc, value));
    });
    logLine("plugin instance created");

    // Host parameters: every Sound parameter of the 16 tracks, the mixer levels / mutes, the send FX.
    const auto& params = ob::digitone2Params();
    byIndex_.assign(params.size() * ob::DeviceModel::kTracks, nullptr);
    auto add = [&](int i, int t, const juce::String& id, const juce::String& name) {
        auto* p = new DeviceParameter(*this, i, t, id, name);
        addParameter(p);
        byIndex_[(size_t)ob::DeviceModel::slotIndex(i, t)] = p;
    };
    for (int t = 0; t < ob::DeviceModel::kTracks; ++t)
        for (size_t i = 0; i < params.size(); ++i) {
            const auto& d = params[i];
            if (d.object == ob::ParamObject::Fx) continue;
            juce::String section = d.sectionName;
            if (const int m = ob::machineOf(d); m >= 0) section = ob::machineName(m);
            if (const int f = ob::filterMachineOf(d); f >= 0) section = juce::String("Filter ") + ob::filterMachineName(f);
            add((int)i, t, juce::String::formatted("t%02d.", t + 1) + d.key,
                "T" + juce::String(t + 1) + " " + (section.isEmpty() ? juce::String() : section + " ") + d.name);
        }
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].object == ob::ParamObject::Fx)
            add((int)i, 0, juce::String("fx.") + params[i].key, juce::String(params[i].sectionName) + " " + params[i].name);
    openMidiOutput();
    midiThread_ = std::thread([this] { midiLoop(); });
    startTimerHz(30);
}

void OpenBridgeProcessor::midiLoop()
{
    while (!midiQuit_.load()) {
        int s1, n1, s2, n2;
        midiFifo_.prepareToRead(midiFifo_.getNumReady(), s1, n1, s2, n2);
        if (n1 + n2 > 0) {
            const juce::ScopedLock l(midiLock_);
            for (int i = 0; i < n1 + n2; ++i) {
                const auto& e = midiEvents_[(size_t)(i < n1 ? s1 + i : s2 + i - n1)];
                if (midiOut_) midiOut_->sendMessageNow(juce::MidiMessage(e.data, e.size));
            }
        }
        midiFifo_.finishedRead(n1 + n2);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

OpenBridgeProcessor::~OpenBridgeProcessor()
{
    stopTimer();
    midiQuit_ = true;
    if (midiThread_.joinable()) midiThread_.join();
    if (session_) session_->releaseAudioOwner(this);
    if (model_) model_->setMidiSender(nullptr);
    model_.reset();
}

void OpenBridgeProcessor::connectSession()
{
    if (session_) session_->releaseAudioOwner(this);
    session_ = ob::DeviceSession::acquire(desc_->pid, wantedId_.toStdString());
    owner_ = session_->claimAudioOwner(this);
    if (model_) model_->setSession(session_);
}

DeviceParameter* OpenBridgeProcessor::parameterFor(int param, int track)
{
    const size_t i = (size_t)ob::DeviceModel::slotIndex(param, track);
    return i < byIndex_.size() ? byIndex_[i] : nullptr;
}

void OpenBridgeProcessor::timerCallback()
{
    if (session_) {
        const juce::String st = session_->statusText();
        if (st != lastStatus_) logLine("device: " + (lastStatus_ = st));
    }
    if (++logTicks_ % 300 == 0) { // every 10 s
        juce::String s;
        s << "process calls " << (juce::int64)processCalls_.load() << ", block " << lastBlock_.load() << ", worst "
          << juce::String(worstProcessMs_.exchange(0.0f), 2) << " ms, owner " << (owner_ ? "yes" : "no")
          << ", control " << (model_->connected() ? "ok" : "no") << ", main peak "
          << juce::String(juce::Decibels::gainToDecibels(mainPeak_.exchange(0.0f), -120.0f), 1) << " dB";
        if (session_)
            if (auto* dev = session_->beginAudio()) {
                auto& st = dev->stats();
                s << ", usb in " << (juce::int64)st.inTransfers.load() << "/" << (juce::int64)st.inErrors.load() << " err, out "
                  << (juce::int64)st.outTransfers.load() << "/" << (juce::int64)st.outErrors.load() << " err, ovf "
                  << (juce::int64)st.toHostOverflows.load() << ", und " << (juce::int64)st.toDeviceUnderruns.load();
                session_->endAudio();
            }
        logLine(s);
    }
    if (const int l = pendingLatency_.load(); l >= 0 && l != getLatencySamples()) setLatencySamples(l);
    model_->poll();
    for (auto& c : model_->takeChanges())
        if (auto* p = parameterFor(c.param, c.track)) p->updateFromDevice(c.raw);
    if (!midiOut_ && ++midiRetry_ % 60 == 0) openMidiOutput(); // every 2 s
}

void OpenBridgeProcessor::openMidiOutput()
{
    // The device's class-compliant USB MIDI port stays available in Overbridge mode.
    for (auto& d : juce::MidiOutput::getAvailableDevices())
        if (d.name.containsIgnoreCase(desc_->name)) {
            auto out = juce::MidiOutput::openDevice(d.identifier);
            if (out) {
                const juce::ScopedLock l(midiLock_);
                midiOut_ = std::move(out);
            }
            return;
        }
}

int OpenBridgeProcessor::outputChannelIndex(const std::string& name) const
{
    for (auto& b : outBuses_)
        if (b.name == name) return b.firstChannel;
    return -1;
}

std::shared_ptr<ob::PresetLibrary> OpenBridgeProcessor::presetLibrary()
{
    if (library_) return library_;
    static std::mutex lock;
    static std::weak_ptr<ob::PresetLibrary> shared;
    std::lock_guard<std::mutex> g(lock);
    library_ = shared.lock();
    if (!library_) {
        const auto cache = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                               .getChildFile(".local/share/OpenBridge/presets-digitone2.tsv");
        library_ = std::make_shared<ob::PresetLibrary>(desc_->name, cache.getFullPathName().toStdString());
        library_->start();
        shared = library_;
    }
    return library_;
}

void OpenBridgeProcessor::sendMidiToDevice(const juce::MidiMessage& m)
{
    const juce::ScopedLock l(midiLock_);
    if (midiOut_) midiOut_->sendMessageNow(m);
}

void OpenBridgeProcessor::selectDevice(const std::string& id)
{
    if (id == wantedId_.toStdString()) return;
    // Connect first, swap under the processing lock only, release the old session afterwards
    // (stopping its USB threads can take a while and must not block the audio thread).
    auto next = ob::DeviceSession::acquire(desc_->pid, id);
    std::shared_ptr<ob::DeviceSession> old;
    {
        const juce::ScopedLock l(getCallbackLock());
        wantedId_ = id;
        old = session_;
        if (old) old->releaseAudioOwner(this);
        session_ = next;
        owner_ = session_->claimAudioOwner(this);
        fromDevice_.reset();
        toDevice_.reset();
    }
    model_->setSession(next);
    old.reset();
}

bool OpenBridgeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    auto check = [](const juce::Array<juce::AudioChannelSet>& got, const std::vector<ob::StereoBus>& want) {
        if (got.size() != (int)want.size()) return false;
        for (int i = 0; i < got.size(); ++i)
            if (!got[i].isDisabled() && got[i].size() != want[(size_t)i].channels) return false;
        return true;
    };
    return check(layouts.inputBuses, ob::groupChannels(desc_->toDevice)) &&
           check(layouts.outputBuses, ob::groupChannels(desc_->toHost));
}

void OpenBridgeProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    logLine("prepareToPlay " + juce::String(sampleRate) + " Hz, block " + juce::String(samplesPerBlock));
    hostRate_ = sampleRate;
    const int announced = std::max(32, samplesPerBlock);
    maxBlock_ = std::max(announced, 8192); // some hosts send larger blocks than announced
    const int outCh = (int)desc_->toHost.size();
    const int inCh = (int)desc_->toDevice.size();
    fromDevice_.prepare(ob::DriftResampler::Mode::Pull, outCh, ob::kDeviceSampleRate, hostRate_, maxBlock_, 1);
    toDevice_.prepare(ob::DriftResampler::Mode::Push, inCh, ob::kDeviceSampleRate, hostRate_, maxBlock_, 1);

    const double usbFrames = 24 * ob::kFramesPerBlock; // one transfer
    const double hostInDevFrames = announced * ob::kDeviceSampleRate / hostRate_;
    fromDevice_.setTargetFill(usbFrames + hostInDevFrames + 48);
    toDevice_.setTargetFill(usbFrames + hostInDevFrames + 48);

    interleavedOut_.assign((size_t)maxBlock_ * outCh, 0.0f);
    interleavedIn_.assign((size_t)maxBlock_ * inCh, 0.0f);
    // Reported from the message thread (timerCallback): a latency change makes the VST3 wrapper
    // call IComponentHandler::restartComponent, which hosts expect on their UI thread.
    pendingLatency_ = fromDevice_.latencyHostFrames() + (int)std::lround(usbFrames * hostRate_ / ob::kDeviceSampleRate);
    if (juce::MessageManager::existsAndIsCurrentThread()) setLatencySamples(pendingLatency_.load());
    prepared_ = true;
}

void OpenBridgeProcessor::releaseResources() { prepared_ = false; }

void OpenBridgeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = std::chrono::steady_clock::now();
    struct Timing {
        OpenBridgeProcessor& p;
        std::chrono::steady_clock::time_point t0;
        ~Timing()
        {
            const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
            float w = p.worstProcessMs_.load();
            while (ms > w && !p.worstProcessMs_.compare_exchange_weak(w, ms)) {}
            ++p.processCalls_;
        }
    } timing{*this, t0};
    const int n = buffer.getNumSamples();
    lastBlock_ = n;
    for (const auto meta : midi) { // to the device's USB MIDI port, through the FIFO
        const auto* d = meta.data;
        if (meta.numBytes < 1 || meta.numBytes > 3 || d[0] < 0x80 || d[0] == 0xF0) continue;
        int s1, n1, s2, n2;
        midiFifo_.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 == 1) {
            auto& e = midiEvents_[(size_t)s1];
            std::memcpy(e.data, d, (size_t)meta.numBytes);
            e.size = meta.numBytes;
        }
        midiFifo_.finishedWrite(n1);
    }
    midi.clear();
    const auto& outBuses = outBuses_;
    const auto& inBuses = inBuses_;
    const int outCh = (int)desc_->toHost.size();
    const int inCh = (int)desc_->toDevice.size();

    ob::UsbDevice* dev = (prepared_ && owner_ && n <= maxBlock_) ? session_->beginAudio() : nullptr;
    if (!dev) {
        buffer.clear();
        return;
    }

    // ---- host -> device (plugin inputs). Read them before the outputs overwrite shared buffers.
    std::fill(interleavedIn_.begin(), interleavedIn_.begin() + (ptrdiff_t)n * inCh, 0.0f);
    for (int b = 0; b < getBusCount(true) && b < (int)inBuses.size(); ++b) {
        auto* bus = getBus(true, b);
        if (!bus->isEnabled()) continue;
        auto view = bus->getBusBuffer(buffer);
        for (int c = 0; c < view.getNumChannels() && c < inBuses[(size_t)b].channels; ++c) {
            const float* src = view.getReadPointer(c);
            float* dst = interleavedIn_.data() + inBuses[(size_t)b].firstChannel + c;
            for (int i = 0; i < n; ++i) dst[(size_t)i * inCh] = src[i];
        }
    }
    toDevice_.push(interleavedIn_.data(), n, dev->toDevice());

    // ---- device -> host (plugin outputs)
    fromDevice_.pull(dev->toHost(), interleavedOut_.data(), n);
    session_->endAudio();
    {
        // peaks per device channel (level meters); channel 0 = Main L also feeds the periodic log
        float pk[64] = {};
        const int chans = std::min(outCh, 64);
        for (int i = 0; i < n; ++i) {
            const float* frame = interleavedOut_.data() + (size_t)i * outCh;
            for (int c = 0; c < chans; ++c) pk[c] = std::max(pk[c], std::fabs(frame[c]));
        }
        for (int c = 0; c < chans; ++c) {
            float cur = channelPeak_[(size_t)c].load();
            while (pk[c] > cur && !channelPeak_[(size_t)c].compare_exchange_weak(cur, pk[c])) {}
        }
        float cur = mainPeak_.load();
        while (pk[0] > cur && !mainPeak_.compare_exchange_weak(cur, pk[0])) {}
    }

    for (int b = 0; b < getBusCount(false) && b < (int)outBuses.size(); ++b) {
        auto* bus = getBus(false, b);
        if (!bus->isEnabled()) continue;
        auto view = bus->getBusBuffer(buffer);
        for (int c = 0; c < view.getNumChannels(); ++c) {
            float* dst = view.getWritePointer(c);
            const int ch = outBuses[(size_t)b].firstChannel + std::min(c, outBuses[(size_t)b].channels - 1);
            const float* src = interleavedOut_.data() + ch;
            for (int i = 0; i < n; ++i) dst[i] = src[(size_t)i * outCh];
        }
    }
}

juce::AudioProcessorEditor* OpenBridgeProcessor::createEditor() { return new OpenBridgeEditor(*this); }

void OpenBridgeProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree s("OpenBridge");
    s.setProperty("device", wantedId_, nullptr);
    s.setProperty("uiZoom", uiZoom, nullptr); // "zoom" (old 1.2 default) is ignored
    s.setProperty("page", uiPage, nullptr);
    s.setProperty("presetBrowser", uiPresetBrowser, nullptr);
    s.setProperty("keyboard", uiKeyboard, nullptr);
    s.setProperty("activeTrack", uiActiveTrack, nullptr);
    s.setProperty("libraryBank", uiLibraryBank, nullptr);
    s.setProperty("tagFilter", (juce::int64)uiTagFilter, nullptr);
    s.setProperty("hideFilters", uiHideFilters, nullptr);
    juce::MemoryOutputStream os(destData, false);
    s.writeToStream(os);
}

void OpenBridgeProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto s = juce::ValueTree::readFromData(data, (size_t)sizeInBytes);
    if (!s.isValid()) return;
    uiZoom = (float)s.getProperty("uiZoom", 1.0f);
    uiPage = (int)s.getProperty("page", 1);
    uiPresetBrowser = (bool)s.getProperty("presetBrowser", false);
    uiKeyboard = (bool)s.getProperty("keyboard", false);
    uiActiveTrack = juce::jlimit(0, 15, (int)s.getProperty("activeTrack", 0));
    uiLibraryBank = juce::jlimit(0, 7, (int)s.getProperty("libraryBank", 0));
    uiTagFilter = (uint32_t)(juce::int64)s.getProperty("tagFilter", 0);
    uiHideFilters = (bool)s.getProperty("hideFilters", false);
    selectDevice(s.getProperty("device", "").toString().toStdString());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OpenBridgeProcessor(); }
