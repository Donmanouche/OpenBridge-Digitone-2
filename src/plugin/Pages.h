// Editor pages, at 100 % (1500 x 1030).
#pragma once

#include "Widgets.h"

#include <memory>
#include <vector>

namespace obui {

int paramIndex(const char* key); // index in ob::digitone2Params(), -1 if unknown

class Page : public juce::Component, private juce::Timer {
public:
    explicit Page(OpenBridgeProcessor& p) : proc_(p) { startTimerHz(20); }
    ~Page() override { stopTimer(); }

protected:
    // Control kinds for make().
    enum Kind { Auto = 0, Value = 1, KnobK = 2, Dot = 3, FaderK = 4, Switch = 5, Radio = 6, HBar = 7, VBar = 8, ChoiceK = 9,
                ChoiceBoxK = 10, Ratio = 11, Send = 12, Hidden = 13, Letter = 14, Inf = 15 };
    // Creates a control of the right kind for the parameter and keeps it for refresh().
    ParamControl* make(juce::Component& parent, int param, int track, juce::Rectangle<int> bounds, int kind = 0,
                       const juce::String& label = {});
    const Tint* tint_ = &Tints::synth; // colours of the controls made next
    virtual void update() {}
    OpenBridgeProcessor& proc_;
    std::vector<std::unique_ptr<ParamControl>> controls_;

private:
    void timerCallback() override;
};

class TrackPage : public Page {
public:
    TrackPage(OpenBridgeProcessor& p, int track);
    void resized() override;

private:
    void update() override;
    void buildMachine();
    void buildFilter();
    void buildLfo();
    void buildSetup();
    void buildModSource(int source);
    static juce::String destinationName(int slot, int machine, int filter);
    void buildFmTone(juce::Component& panel);
    void buildFmDrum(juce::Component& panel);
    void addAlgorithmArrows(juce::Component& panel, const char* key, int maxIndex, juce::Rectangle<int> left, juce::Rectangle<int> right);
    void buildWaveTone(juce::Component& panel);
    void buildSwarmer(juce::Component& panel);
    void buildMidi(juce::Component& panel);
    // helpers for the machine pages (layout units x 1.25)
    Group* machineGroup(juce::Component& panel, const juce::String& title, juce::Rectangle<int> b);
    void machineGraph(juce::Component& panel, Graph::Kind kind, std::vector<std::string> keys, juce::Rectangle<int> b);
    void machineLabel(juce::Component& panel, const juce::String& text, juce::Rectangle<int> b, float size = 15.0f,
                      juce::uint32 colour = 0);
    // group box with parameters in a grid (columns x rows of value boxes, toggles as switches)
    void paramGroup(juce::Component& panel, const juce::String& title, juce::Rectangle<int> box,
                    std::initializer_list<std::pair<const char*, const char*>> params, int columns);
    void place(juce::Component& parent, juce::Rectangle<int> area, std::vector<int> params, int columns, int rowHeight);
    Graph* addGraph(juce::Component& parent, Graph::Kind kind, juce::Rectangle<int> bounds, const Tint& t, int index = 0,
                    bool fill = false);
    Group* addGroup(juce::Component& parent, const juce::String& title, juce::Rectangle<int> bounds, const Tint& t);
    void showSaveMenu();
    void removeControls(std::vector<ParamControl*>& list);
    void removeOwned(std::vector<juce::Component*>& list);

    std::vector<std::unique_ptr<Graph>> graphs_;
    std::vector<std::unique_ptr<Group>> groups_;
    std::vector<std::unique_ptr<juce::Component>> extras_;

    const int track_;
    bool built_ = false;
    int builtMachine_ = -2, builtFilter_ = -2, builtLfo_ = -1, builtSetup_ = -1;
    Panel synth_{"SYNTH"}, preset_{"PRESET"}, setup_{"SETUP"}, baseWidth_{"BASE / WIDTH FILTER"}, filter_{"FILTER"},
        amp_{"AMP"}, modulation_{"MODULATION"}, fx_{"FX"};
    std::vector<std::unique_ptr<StripTab>> lfoTabs_, setupTabs_;
    std::vector<ParamControl*> machineControls_, filterControls_, lfoControls_, setupControls_;
    std::vector<juce::Component*> machineParts_, filterParts_, lfoParts_, setupParts_; // groups / graphs / labels of the parts
    TextLine soundName_, waiting_, machineLabel_, soundTags_;
    juce::TextButton clearButton_{"Clear"}, loadButton_{"Load..."}, saveButton_{"Save..."};
    juce::ComboBox machineBox_, filterBox_;
};

class SendFxPage : public Page {
public:
    explicit SendFxPage(OpenBridgeProcessor& p);
    void resized() override;

private:
    std::vector<std::unique_ptr<Panel>> panels_;
    std::vector<std::unique_ptr<Group>> groups_;
    std::vector<std::unique_ptr<Graph>> graphs_;
    std::vector<std::unique_ptr<juce::Component>> extras_;
};

class MixerPage : public Page {
public:
    explicit MixerPage(OpenBridgeProcessor& p);
    void resized() override;

private:
    Panel routing_{"COMPRESSOR ROUTING"}, compressor_{"COMPRESSOR"}, extIn_{"EXT-IN"}, mixer_{"MIXER"};
    std::vector<std::unique_ptr<Group>> groups_;
    std::vector<std::unique_ptr<Graph>> graphs_;
    std::vector<std::unique_ptr<LevelMeter>> meters_;
    std::vector<std::unique_ptr<juce::Component>> extras_;
};

class KitPage : public Page {
public:
    explicit KitPage(OpenBridgeProcessor& p);
    void paint(juce::Graphics&) override;

private:
    void update() override { repaint(); }
};

} // namespace obui
