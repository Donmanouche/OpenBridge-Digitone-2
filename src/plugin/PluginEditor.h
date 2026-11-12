#pragma once

#include "Pages.h"
#include "PresetBrowser.h"
#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace obui {

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    LookAndFeel();
    void drawComboBox(juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
};

class HeaderBar : public juce::Component {
public:
    explicit HeaderBar(OpenBridgeProcessor& p);
    void paint(juce::Graphics&) override;
    void resized() override;
    void refreshDevices();
    void setPresetBrowser(bool on);
    std::function<void(float)> onZoom;
    std::function<void(int)> onPage;      // 0 = kit editor, 1 = preset browser
    std::function<void()> onKeyboard;

private:
    OpenBridgeProcessor& proc_;
    juce::ComboBox zoom_, device_;
    // keyboard icon
    struct KeyboardButton : juce::Button {
        KeyboardButton() : juce::Button("keyboard") {}
        void paintButton(juce::Graphics&, bool over, bool down) override;
    } keyboard_;
    juce::TextButton kitEditor_{"Kit Editor"}, presetBrowser_{"Preset Browser"};
    std::vector<std::string> deviceIds_;
};

class TrackTabs : public juce::Component {
public:
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void setSelected(int i)
    {
        selected_ = i;
        repaint();
    }
    std::function<void(int)> onChange;
    static juce::StringArray names();

private:
    juce::Rectangle<int> tabBounds(int i) const;
    int selected_ = 1;
};

class Footer : public juce::Component, private juce::Timer {
public:
    explicit Footer(OpenBridgeProcessor& p) : proc_(p) { startTimerHz(4); }
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override { repaint(); }
    OpenBridgeProcessor& proc_;
};

} // namespace obui

class OpenBridgeEditor : public juce::AudioProcessorEditor, private juce::MidiKeyboardState::Listener {
public:
    explicit OpenBridgeEditor(OpenBridgeProcessor&);
    ~OpenBridgeEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

    void openPresetBrowser();

    static constexpr int kBaseWidth = 1500;
    static constexpr int kBaseHeight = 1030;
    // The layout is drawn at 1500 x 1030; 100 % zoom shows it at 80 % of that (1200 x 824).
    static constexpr float kScale = 0.8f;

private:
    void applyZoom(float z);
    void showPage(int page);
    void handleNoteOn(juce::MidiKeyboardState*, int ch, int note, float vel) override;
    void handleNoteOff(juce::MidiKeyboardState*, int ch, int note, float vel) override;

    OpenBridgeProcessor& proc_;
    obui::LookAndFeel lnf_;
    juce::TooltipWindow tooltips_{this, 600};
    juce::Component content_;
    obui::HeaderBar header_;
    obui::TrackTabs tabs_;
    obui::Footer footer_;
    std::unique_ptr<juce::Component> page_;
    juce::MidiKeyboardState keyState_;
    juce::MidiKeyboardComponent keyboard_{keyState_, juce::MidiKeyboardComponent::horizontalKeyboard};
    bool showKeyboard_ = false;
    bool presetBrowser_ = false;
};
