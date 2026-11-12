// Controls in the style of the Overbridge editors, bound to one device parameter on one track.
// Colours and shapes of the editor.
#pragma once

#include "PluginProcessor.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace obui {

struct Palette {
    static constexpr juce::uint32 background = 0xff1e1e1e;
    static constexpr juce::uint32 header = 0xff2b2b2b;     // top bar, panel title strips, footer
    static constexpr juce::uint32 panel = 0xff1e1e1e;      // panel body
    static constexpr juce::uint32 panelHeader = 0xff2b2b2b;
    static constexpr juce::uint32 panelBorder = 0xff5a5a5a;
    static constexpr juce::uint32 panelTitle = 0xff6b6b6b;
    static constexpr juce::uint32 panelInner = 0xff262a29;
    static constexpr juce::uint32 tab = 0xff1e1e1e;
    static constexpr juce::uint32 tabActive = 0xff1e1e1e;
    static constexpr juce::uint32 tabBorder = 0xff3a3a3a;
    static constexpr juce::uint32 tabBorderActive = 0xff646464;
    static constexpr juce::uint32 accent = 0xff4fb3c1;     // Digitone II cyan (title)
    static constexpr juce::uint32 text = 0xffe6e6e6;
    static constexpr juce::uint32 textDim = 0xff6d6d6d;
    static constexpr juce::uint32 control = 0xff9ac0b5;
    static constexpr juce::uint32 outline = 0xff646464;   // combo boxes, buttons
    static constexpr juce::uint32 ampTint = 0xffcd7b81;
    static constexpr juce::uint32 filterTint = 0xffe4af8f;
    static constexpr juce::uint32 meterGreen = 0xff7ccf9a;
};

// Colour set of a section: group box fill, values / graphs, group titles.
struct Tint {
    juce::uint32 group, accent, title;
};
namespace Tints {
inline constexpr Tint synth{0xff262a29, 0xff9ac0b5, 0xffa3beb7};
inline constexpr Tint filter{0xff2d2825, 0xffe4af8f, 0xffe4af8f};
inline constexpr Tint amp{0xff2c2221, 0xffcd7b81, 0xffcd7b81};
inline constexpr Tint mod{0xff2f2b2f, 0xffe2dbe4, 0xff9a949b};
inline constexpr Tint fx{0xff282d30, 0xff748ca6, 0xffa9bccf};
inline constexpr Tint plain{0xff1e1e1e, 0xffe6e6e6, 0xffe6e6e6};
} // namespace Tints

juce::Font uiFont(float size, bool bold = false);

// Base for controls bound to (param, track).
class ParamControl : public juce::Component, public juce::SettableTooltipClient {
public:
    ParamControl(OpenBridgeProcessor& p, int param, int track);
    void refresh(); // reads the model; repaints when changed

    int raw() const { return raw_; }
    int paramIndex() const { return param_; }
    int trackIndex() const { return track_; }
    const ob::ParamDef& def() const;
    bool active() const { return active_; }
    juce::String valueText() const;
    juce::String label;
    const Tint* tint = &Tints::synth;

protected:
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void setRaw(int raw);
    float norm() const;
    juce::String labelText() const { return label.isNotEmpty() ? label : juce::String(def().name); }
    float alpha() const { return active_ ? 1.0f : 0.35f; }
    juce::Colour accent() const { return juce::Colour(tint->accent).withMultipliedAlpha(alpha()); }
    juce::Colour textColour() const { return juce::Colour(Palette::text).withMultipliedAlpha(alpha()); }

    OpenBridgeProcessor& proc_;
    const int param_, track_;
    int raw_ = 0;
    bool active_ = true;
    float dragStartNorm_ = 0;
    bool gesture_ = false;
};

// "Label / [ value ] / position bar" (most Overbridge parameters). boxed: the square frames of
// the FM ratios.
class ValueBox : public ParamControl {
public:
    using ParamControl::ParamControl;
    void paint(juce::Graphics&) override;
    bool boxed = false;
    bool showLabel = true;
    int ratioPart = 0; // FM ratio B: 1 = B2, 2 = B1 (both edit the same raw value)

protected:
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::String shownText() const;
    void stepPart(int steps);
    int partStart_ = 0;
};

// Knob: thick ring with a pointer, label above and value below (pan / volume, FX, LFO).
class Knob : public ParamControl {
public:
    using ParamControl::ParamControl;
    void paint(juce::Graphics&) override;
};

// Two-state control. Dot: "• label"; Switch: the two-cell switch of Reset / Trig / Inf;
// Radio: "• Pre Filter / • Post Filter".
class Toggle : public ParamControl {
public:
    enum class Style { Dot, Switch, Radio, Hidden, Letter, Inf };
    using ParamControl::ParamControl;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override {}
    Style style = Style::Dot;
    juce::StringArray radioLabels{"Pre Filter", "Post Filter"};
    juce::uint32 onColour = 0xffdf575e; // Letter style (mixer M / S buttons)
    int restoreRaw = 0x4000;            // Inf style: value given back when "Inf" is switched off
};

// FX send icons (delay: ring with a dot, reverb: cube, chorus: cone), value below.
class SendIcon : public ParamControl {
public:
    using ParamControl::ParamControl;
    void paint(juce::Graphics&) override;
    int icon = 0;
};

// Vertical fader (track level): tinted column with a white handle.
class Fader : public ParamControl {
public:
    using ParamControl::ParamControl;
    void paint(juce::Graphics&) override;
    bool showValue = true;
    void mouseDrag(const juce::MouseEvent&) override;
};

// Slider bar: horizontal (X-Y Mix, setup values) or vertical (key tracking), value box below.
class Bar : public ParamControl {
public:
    using ParamControl::ParamControl;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    bool vertical = false;
    bool valueRight = false; // value printed to the right (setup page) instead of below
    bool compact = false;    // only the bar (mixer pan)
};

// Row of options, the selected one bright (algorithm 1..8, LFO trig mode, phase reset, waveforms).
class Choice : public ParamControl {
public:
    using ParamControl::ParamControl;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override {}
    juce::StringArray options;
    int firstInRow = 0;     // options before this index are shown as the large value instead (phase reset "OFF")
    bool bigValue = false;  // current option in large type above the row
    bool waveIcons = false; // LFO waveform glyphs instead of text
    bool vertical = false;  // options stacked vertically
    int columns = 0;        // > 0: grid with this many columns
    std::vector<int> order; // display position -> option index (default: identity)
    int selected() const;

private:
    juce::Rectangle<float> cellBounds(int i, int n, juce::Rectangle<float> r) const;
};

// Drop-down list bound to a parameter (LFO destination).
class ChoiceBox : public ParamControl {
public:
    ChoiceBox(OpenBridgeProcessor& p, int param, int track);
    void resized() override;
    void paint(juce::Graphics&) override;
    void setOptions(const juce::StringArray& names, const std::vector<int>& values);
    void refreshSelection();

private:
    juce::ComboBox box_;
    std::vector<int> values_;
    int shown_ = -1;
};

// Read-only drawing of an envelope or a filter shape, from the model's values.
class Graph : public juce::Component, private juce::Timer {
public:
    enum class Kind { FilterEnvelope, AmpEnvelope, BaseWidth, FilterResponse, FmEnvA, FmEnvB, Harmonics, Detune, Feedback, Lfo, Algorithm, DrumAlgorithm, OscMod,
                      SimpleEnvelope, BandPass, Waveform, Decoration,
                      OpEnvelope, Ruler, PhaseStart, FeedbackIcon, FoldIcon, SectionBar,
                      CrossIcon, TableIcon, DriftIcon, SwarmLines, AnimIcon, NoiseDots,
                      DelayEcho, ReverbSpace, Shelf, ChorusWidth, HighPass, CompCurve };
    Graph(OpenBridgeProcessor& p, int track, Kind kind, int index = 0);
    // SimpleEnvelope: keys {attack or "", hold, decay}; BandPass: {base, width}; Waveform: {wave};
    // OpEnvelope: {decay, end, level}; Ruler / PhaseStart: {value}; SectionBar: {title}
    Graph(OpenBridgeProcessor& p, int track, Kind kind, std::vector<std::string> keys);
    void paint(juce::Graphics&) override;
    const Tint* tint = &Tints::synth;
    bool fill = true;

private:
    void timerCallback() override;
    float value(const char* key) const; // 0..1
    int raw(const char* key) const;
    OpenBridgeProcessor& proc_;
    const int track_;
    const Kind kind_;
    const int index_;
    std::vector<int> params_;
    std::vector<int> last_;
    std::vector<std::string> keys_;
};

// Stereo level meter (two bars, dB scale, peak hold) fed by the processor's channel peaks.
class LevelMeter : public juce::Component, private juce::Timer {
public:
    LevelMeter(OpenBridgeProcessor& p, int firstChannel) : proc_(p), channel_(firstChannel) { startTimerHz(30); }
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;
    OpenBridgeProcessor& proc_;
    const int channel_;
    float level_[2] = {}, hold_[2] = {};
    int holdFrames_[2] = {};
};

// Tinted group box inside a panel, optional title on top ("Algorithm", "Ratios", "Key tracking").
class Group : public juce::Component {
public:
    Group(juce::String title, const Tint& t) : title_(std::move(title)), tint_(&t) { setInterceptsMouseClicks(false, true); }
    void paint(juce::Graphics&) override;
    void setTitle(juce::String t) { title_ = std::move(t), repaint(); }
    juce::StringArray extraTitles; // more titles on the same line, centred in equal columns ("Ratios", "Ratio Offset")

private:
    juce::String title_;
    const Tint* tint_;
};

// Static text, optionally in a box (sound name in the PRESET panel).
class TextLine : public juce::Component {
public:
    void setText(const juce::String& t)
    {
        if (t != text_) {
            text_ = t;
            repaint();
        }
    }
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        if (boxed) {
            g.setColour(juce::Colour(Palette::background));
            g.fillRect(r);
            g.setColour(juce::Colour(Palette::outline));
            g.drawRect(r, 2);
            r.reduce(12, 0);
        }
        g.setColour(colour ? juce::Colour(colour) : juce::Colour(dim ? Palette::textDim : Palette::text));
        g.setFont(uiFont(size, bold));
        g.drawText(text_, r, justification);
    }
    bool boxed = false, dim = false, bold = false;
    float size = 16.0f;
    juce::uint32 colour = 0;
    juce::Justification justification = juce::Justification::centredLeft;

private:
    juce::String text_;
};

// Titled panel ("SYNTH", "FILTER", ...): grey frame, dark title strip.
class Panel : public juce::Component {
public:
    static constexpr int kHeader = 28;
    explicit Panel(juce::String title) : title_(std::move(title)) {}
    void paint(juce::Graphics&) override;
    juce::Rectangle<int> content() const { return getLocalBounds().withTrimmedTop(kHeader + 2).reduced(9, 8); }
    void setTitle(juce::String t)
    {
        title_ = std::move(t);
        repaint();
    }
    juce::String rightText; // e.g. machine name

private:
    juce::String title_;
};

// Flat tab buttons in a panel's title strip (SETUP: TRK VOICE ...; MODULATION: LFO1 LFO2 ...).
class StripTab : public juce::Button {
public:
    explicit StripTab(const juce::String& name) : juce::Button(name) { setClickingTogglesState(true); }
    void paintButton(juce::Graphics& g, bool over, bool) override
    {
        const bool on = getToggleState();
        if (on) {
            g.setColour(juce::Colour(0xff212121));
            g.fillRect(getLocalBounds());
        }
        g.setColour(juce::Colour(on ? 0xfff7f7f7 : 0xff6d6d6d).brighter(over && !on ? 0.3f : 0.0f));
        g.setFont(uiFont(15.0f, true));
        g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
    }
};

} // namespace obui
