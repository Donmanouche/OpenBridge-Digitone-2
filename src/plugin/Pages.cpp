#include "Pages.h"
#include "PluginEditor.h"

#include <algorithm>
#include <functional>
#include <cstring>

namespace obui {

using C = juce::Colour;

int paramIndex(const char* key)
{
    const auto* p = ob::findParam(key);
    return p ? (int)(p - ob::digitone2Params().data()) : -1;
}

namespace {

std::vector<int> paramsWhere(const std::function<bool(const ob::ParamDef&)>& pred)
{
    std::vector<int> v;
    const auto& all = ob::digitone2Params();
    for (size_t i = 0; i < all.size(); ++i)
        if (pred(all[i])) v.push_back((int)i);
    std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return all[(size_t)a].knob < all[(size_t)b].knob; });
    return v;
}

bool isToggle(const ob::ParamDef& d) { return d.format == ob::ParamFormat::OnOff || (d.max - d.min) <= 256; }

std::vector<int> keys(std::initializer_list<const char*> k)
{
    std::vector<int> v;
    for (auto* s : k)
        if (int i = paramIndex(s); i >= 0) v.push_back(i);
    return v;
}

} // namespace

// ------------------------------------------------------------------------------------------ Page
ParamControl* Page::make(juce::Component& parent, int param, int track, juce::Rectangle<int> bounds, int kind,
                         const juce::String& label)
{
    if (param < 0) return nullptr;
    const auto& d = ob::digitone2Params()[(size_t)param];
    std::unique_ptr<ParamControl> c;
    if (kind == Auto) kind = isToggle(d) ? Dot : Value;
    switch (kind) {
    case KnobK: c = std::make_unique<Knob>(proc_, param, track); break;
    case FaderK: c = std::make_unique<Fader>(proc_, param, track); break;
    case Dot:
    case Switch:
    case Radio:
    case Hidden:
    case Letter:
    case Inf: {
        auto t = std::make_unique<Toggle>(proc_, param, track);
        t->style = kind == Switch ? Toggle::Style::Switch : kind == Radio ? Toggle::Style::Radio : kind == Hidden ? Toggle::Style::Hidden
                   : kind == Letter                                                          ? Toggle::Style::Letter
                   : kind == Inf                                                             ? Toggle::Style::Inf
                                                                                             : Toggle::Style::Dot;
        c = std::move(t);
        break;
    }
    case HBar:
    case VBar: {
        auto b = std::make_unique<Bar>(proc_, param, track);
        b->vertical = kind == VBar;
        c = std::move(b);
        break;
    }
    case ChoiceK: c = std::make_unique<Choice>(proc_, param, track); break;
    case ChoiceBoxK: c = std::make_unique<ChoiceBox>(proc_, param, track); break;
    case Send: c = std::make_unique<SendIcon>(proc_, param, track); break;
    case Ratio: {
        auto b = std::make_unique<ValueBox>(proc_, param, track);
        b->boxed = true;
        c = std::move(b);
        break;
    }
    default: c = std::make_unique<ValueBox>(proc_, param, track);
    }
    c->label = label;
    c->tint = tint_;
    c->setBounds(bounds);
    parent.addAndMakeVisible(*c);
    controls_.push_back(std::move(c));
    return controls_.back().get();
}

void Page::timerCallback()
{
    update();
    for (auto& c : controls_) c->refresh();
}

// ------------------------------------------------------------------------------------------ TrackPage
// Layout in page units (1795 x 1232 at 120 %, i.e.
// this 1500 x 1030 layout x 1.197); page coordinates start under the track tabs (y 90).
namespace {
juce::Rectangle<int> centred(int cx, int y, int w, int h) { return {cx - w / 2, y, w, h}; }

// Value outside the host parameters (sound setup, kit voice configuration): get / set functions.
using Getter = std::function<int()>;
using Setter = std::function<void(int)>;

// Bar with the value on the right, as in the SETUP panel.
class SetupBar : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
public:
    SetupBar(Getter get, Setter set, int lo, int hi, int step) : get_(std::move(get)), set_(std::move(set)), lo_(lo), hi_(hi), step_(step)
    {
        startTimerHz(10);
    }
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const int v = get_();
        const bool known = v > -1000;
        auto bar = r.withWidth(r.getWidth() - 56).withSizeKeepingCentre(r.getWidth() - 56, 12);
        g.setColour(juce::Colour(0xff3a3a3a));
        g.fillRect(bar);
        if (known) {
            const float n = juce::jlimit(0.0f, 1.0f, (float)(v - lo_) / (float)(hi_ - lo_));
            g.setColour(juce::Colour(0xfff0f0f0));
            g.fillRect(bar.withWidth(std::max(8.0f, bar.getWidth() * n)));
        }
        g.setColour(juce::Colour(Palette::text));
        g.setFont(uiFont(16.0f));
        g.drawText(known ? juce::String(v / step_) : juce::String("-"), r.removeFromRight(36), juce::Justification::centredLeft);
    }
    void mouseDown(const juce::MouseEvent& e) override { set(e); }
    void mouseDrag(const juce::MouseEvent& e) override { set(e); }

private:
    void set(const juce::MouseEvent& e)
    {
        if (get_() <= -1000) return;
        const float n = juce::jlimit(0.0f, 1.0f, (float)e.x / (float)(getWidth() - 56));
        const int v = juce::jlimit(lo_, hi_, lo_ + (int)std::lround(n * (float)(hi_ - lo_) / (float)step_) * step_);
        if (v != get_()) {
            set_(v);
            repaint();
        }
    }
    void timerCallback() override
    {
        const int v = get_();
        if (v != last_) last_ = v, repaint();
    }
    Getter get_;
    Setter set_;
    const int lo_, hi_, step_;
    int last_ = -2000;
};

// Drop-down list; option i is value first + i.
class SetupCombo : public juce::ComboBox, private juce::Timer {
public:
    SetupCombo(Getter get, Setter set, const juce::StringArray& options, int first = 0)
        : get_(std::move(get)), set_(std::move(set)), first_(first)
    {
        addItemList(options, 1);
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(Palette::background));
        onChange = [this] {
            const int v = getSelectedItemIndex() + first_;
            if (getSelectedItemIndex() >= 0 && get_() > -1000 && v != get_()) set_(v);
        };
        timerCallback();
        startTimerHz(10);
    }

private:
    void timerCallback() override
    {
        const int v = get_();
        if (v != last_) {
            last_ = v;
            if (v <= -1000) setSelectedItemIndex(-1, juce::dontSendNotification);
            else setSelectedItemIndex(v - first_, juce::dontSendNotification);
        }
    }
    Getter get_;
    Setter set_;
    const int first_;
    int last_ = -2000;
};

// Drop-down list with explicit values per item.
class ValueCombo : public juce::ComboBox, private juce::Timer {
public:
    ValueCombo(Getter get, Setter set, const juce::StringArray& names, std::vector<int> values)
        : get_(std::move(get)), set_(std::move(set)), values_(std::move(values))
    {
        addItemList(names, 1);
        onChange = [this] {
            const int i = getSelectedItemIndex();
            if (i >= 0 && i < (int)values_.size() && get_() > -1000 && values_[(size_t)i] != get_()) set_(values_[(size_t)i]);
        };
        timerCallback();
        startTimerHz(10);
    }

private:
    void timerCallback() override
    {
        const int v = get_();
        if (v == last_) return;
        last_ = v;
        int idx = -1;
        for (size_t i = 0; i < values_.size(); ++i)
            if (values_[i] == v) idx = (int)i;
        setSelectedItemIndex(idx, juce::dontSendNotification);
        if (idx < 0 && v > -1000) setText("Slot " + juce::String(v), juce::dontSendNotification);
    }
    Getter get_;
    Setter set_;
    std::vector<int> values_;
    int last_ = -2000;
};

// Signed 8.8 value ("Depth" of the modulation sources): bracketed value, drag to change
// (shift: fine), double-click: 0.
class SignedValue : public juce::Component, private juce::Timer {
public:
    SignedValue(Getter get, Setter set, const Tint& t, juce::String label)
        : get_(std::move(get)), set_(std::move(set)), tint_(&t), label_(std::move(label))
    {
        startTimerHz(10);
    }
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour(juce::Colour(Palette::text));
        g.setFont(uiFont(15.0f));
        g.drawText(label_, r.removeFromTop(22), juce::Justification::centred);
        const int v = get_();
        const juce::String text = v <= -1000 ? juce::String("-") : juce::String(formatSigned(v));
        auto box = r.removeFromTop(24).withSizeKeepingCentre(70, 22);
        const juce::Colour c(tint_->accent);
        g.setColour(c.withAlpha(0.4f));
        g.drawRect(box.withWidth(4).withHeight(1));
        g.drawLine(box.getX(), box.getY(), box.getX(), box.getBottom(), 1.3f);
        g.drawLine(box.getRight(), box.getY(), box.getRight(), box.getBottom(), 1.3f);
        g.drawLine(box.getX(), box.getY(), box.getX() + 4, box.getY(), 1.3f);
        g.drawLine(box.getX(), box.getBottom(), box.getX() + 4, box.getBottom(), 1.3f);
        g.drawLine(box.getRight() - 4, box.getY(), box.getRight(), box.getY(), 1.3f);
        g.drawLine(box.getRight() - 4, box.getBottom(), box.getRight(), box.getBottom(), 1.3f);
        g.setColour(c);
        g.setFont(uiFont(16.5f));
        g.drawText(text, box, juce::Justification::centred);
        if (v > -1000) {
            auto line = juce::Rectangle<float>(box.getX() + 4, box.getBottom() + 4, box.getWidth() - 8, 2);
            g.setColour(c.withAlpha(0.45f));
            g.fillRect(line);
            g.setColour(c);
            const float n = (float)(v + 32768) / 65535.0f;
            g.fillEllipse(line.getX() + line.getWidth() * n - 2.5f, line.getY() - 1.5f, 5, 5);
        }
    }
    void mouseDown(const juce::MouseEvent&) override { start_ = get_(); }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (start_ <= -1000) return;
        const int step = e.mods.isShiftDown() ? 8 : 256;
        const int v = juce::jlimit(-32768, 32767, start_ - e.getDistanceFromDragStartY() / 2 * step);
        if (v != get_()) set_(v), repaint();
    }
    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (get_() > -1000) set_(0), repaint();
    }

private:
    static std::string formatSigned(int v)
    {
        // signed, two decimals of the fine byte
        if (v == 0) return "0.00";
        const int a = std::abs(v), frac = std::max((a & 0xff) * 100 >> 8, a < 8 ? 1 : 0);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%s%d.%02d", v < 0 ? "-" : "", a >> 8, frac);
        return buf;
    }
    void timerCallback() override
    {
        const int v = get_();
        if (v != last_) last_ = v, repaint();
    }
    Getter get_;
    Setter set_;
    const Tint* tint_;
    juce::String label_;
    int start_ = -2000, last_ = -2000;
};

// LAYER: one button per track, lit when that track is triggered together with this one.
class TrigMaskGrid : public juce::Component, private juce::Timer {
public:
    TrigMaskGrid(ob::DeviceModel& m, int track) : model_(m), track_(track) { startTimerHz(10); }
    juce::Rectangle<int> cell(int t) const
    {
        const int w = getWidth() / 4, h = getHeight() / 4;
        return juce::Rectangle<int>((t % 4) * w, (t / 4) * h, w, h).reduced(4);
    }
    void paint(juce::Graphics& g) override
    {
        const int mask = model_.trigMask(track_);
        for (int t = 0; t < 16; ++t) {
            auto r = cell(t);
            const bool self = t == track_, on = mask > -1000 && ((mask >> t) & 1);
            g.setColour(juce::Colour(on || self ? 0xff285765 : 0xff2a2a2a));
            g.fillRect(r);
            if (self) {
                g.setColour(juce::Colour(0xffeb9f6a));
                g.drawRect(r, 2);
            }
            g.setColour(juce::Colour(on || self ? Palette::text : Palette::textDim));
            g.setFont(uiFont(14.0f));
            g.drawText("TRK " + juce::String(t + 1), r, juce::Justification::centred);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        const int mask = model_.trigMask(track_);
        if (mask <= -1000) return;
        for (int t = 0; t < 16; ++t)
            if (t != track_ && cell(t).contains(e.getPosition())) {
                model_.setTrigMask(track_, mask ^ (1 << t));
                repaint();
            }
    }

private:
    void timerCallback() override
    {
        const int m = model_.trigMask(track_);
        if (m != last_) last_ = m, repaint();
    }
    ob::DeviceModel& model_;
    const int track_;
    int last_ = -2000;
};
} // namespace

Graph* TrackPage::addGraph(juce::Component& parent, Graph::Kind kind, juce::Rectangle<int> bounds, const Tint& t, int index, bool fill)
{
    auto gph = std::make_unique<Graph>(proc_, track_, kind, index);
    gph->tint = &t;
    gph->fill = fill;
    gph->setBounds(bounds);
    parent.addAndMakeVisible(*gph);
    graphs_.push_back(std::move(gph));
    return graphs_.back().get();
}

Group* TrackPage::addGroup(juce::Component& parent, const juce::String& title, juce::Rectangle<int> bounds, const Tint& t)
{
    auto g = std::make_unique<Group>(title, t);
    g->setBounds(bounds);
    parent.addAndMakeVisible(*g);
    g->toBack();
    groups_.push_back(std::move(g));
    return groups_.back().get();
}

void TrackPage::removeControls(std::vector<ParamControl*>& list)
{
    for (auto* c : list)
        controls_.erase(std::remove_if(controls_.begin(), controls_.end(), [c](auto& u) { return u.get() == c; }), controls_.end());
    list.clear();
}

void TrackPage::removeOwned(std::vector<juce::Component*>& list)
{
    for (auto* c : list) {
        graphs_.erase(std::remove_if(graphs_.begin(), graphs_.end(), [c](auto& u) { return u.get() == c; }), graphs_.end());
        groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [c](auto& u) { return u.get() == c; }), groups_.end());
        extras_.erase(std::remove_if(extras_.begin(), extras_.end(), [c](auto& u) { return u.get() == c; }), extras_.end());
    }
    list.clear();
}

TrackPage::TrackPage(OpenBridgeProcessor& p, int track) : Page(p), track_(track)
{
    for (auto* panel : {&synth_, &preset_, &setup_, &baseWidth_, &filter_, &amp_, &modulation_, &fx_})
        addAndMakeVisible(*panel);
    const char* modTabs[] = {"LFO1", "LFO2", "LFO3", "KEY TR.", "PITCH BEND", "VELOCITY", "MOD WHEEL", "BREATH", "AFTERTOUCH"};
    for (int i = 0; i < 9; ++i) {
        auto b = std::make_unique<StripTab>(modTabs[i]);
        b->setRadioGroupId(77);
        b->onClick = [this, i] {
            proc_.uiLfo = i;
            buildLfo();
        };
        modulation_.addAndMakeVisible(*b);
        lfoTabs_.push_back(std::move(b));
    }
    lfoTabs_[(size_t)juce::jlimit(0, 8, proc_.uiLfo)]->setToggleState(true, juce::dontSendNotification);
    const char* setupTabs[] = {"TRK", "VOICE", "PORTAMENTO", "LAYER"};
    for (int i = 0; i < 4; ++i) {
        auto b = std::make_unique<StripTab>(setupTabs[i]);
        b->setRadioGroupId(78);
        b->onClick = [this, i] {
            proc_.uiSetupTab = i;
            buildSetup();
        };
        setup_.addAndMakeVisible(*b);
        setupTabs_.push_back(std::move(b));
    }
    setupTabs_[(size_t)juce::jlimit(0, 3, proc_.uiSetupTab)]->setToggleState(true, juce::dontSendNotification);
}

void TrackPage::place(juce::Component& parent, juce::Rectangle<int> area, std::vector<int> params, int columns, int rowHeight)
{
    const int w = area.getWidth() / columns;
    for (size_t i = 0; i < params.size(); ++i) {
        const auto r = juce::Rectangle<int>(area.getX() + ((int)i % columns) * w, area.getY() + ((int)i / columns) * rowHeight, w,
                                            rowHeight - 6);
        const auto& d = ob::digitone2Params()[(size_t)params[i]];
        if (isToggle(d)) make(parent, params[i], track_, r.withSizeKeepingCentre(std::min(w, 140), 22), Dot);
        else make(parent, params[i], track_, r.withSizeKeepingCentre(std::min(w, 110), std::min(rowHeight - 6, 64)), Value);
    }
}

void TrackPage::resized()
{
    synth_.setBounds(3, 0, 1148, 463);
    preset_.setBounds(1156, 0, 338, 157);
    setup_.setBounds(1156, 162, 338, 301);
    baseWidth_.setBounds(3, 469, 285, 239);
    filter_.setBounds(292, 469, 621, 239);
    amp_.setBounds(918, 469, 576, 239);
    modulation_.setBounds(3, 714, 782, 179);
    fx_.setBounds(790, 714, 704, 179);
    if (built_) return; // built once; machine / filter / LFO / setup parts rebuild in update()
    built_ = true;

    // ---- PRESET: name, [Clear] [Load...] [Save...], labels
    soundName_.boxed = true;
    soundName_.size = 15.5f;
    soundName_.setBounds(9, 39, 318, 30);
    preset_.addAndMakeVisible(soundName_);
    clearButton_.setBounds(9, 79, 99, 29);
    loadButton_.setBounds(118, 79, 99, 29);
    saveButton_.setBounds(228, 79, 99, 29);
    clearButton_.setTooltip("Reset this track's sound to the device's default sound");
    clearButton_.onClick = [this] { proc_.model().clearSound(track_); };
    loadButton_.setTooltip("Open the Preset Browser to load a preset into this track");
    saveButton_.setTooltip("Saving to the +Drive is not available yet");
    loadButton_.onClick = [this] {
        proc_.uiActiveTrack = track_;
        if (auto* ed = findParentComponentOfClass<OpenBridgeEditor>()) ed->openPresetBrowser();
    };
    saveButton_.onClick = [this] { showSaveMenu(); };
    for (auto* b : {&clearButton_, &loadButton_, &saveButton_}) {
        b->getProperties().set("outlined", true);
        b->setColour(juce::TextButton::textColourOffId, juce::Colour(Palette::text));
        preset_.addAndMakeVisible(*b);
    }
    soundTags_.size = 15.0f;
    soundTags_.setBounds(9, 118, 320, 24);
    preset_.addAndMakeVisible(soundTags_);

    // ---- SETUP tabs in the title strip
    const int tabX[] = {76, 122, 178, 284}, tabW[] = {38, 42, 93, 44};
    for (int i = 0; i < 4; ++i) setupTabs_[(size_t)i]->setBounds(tabX[i], 3, tabW[i] + 8, Panel::kHeader - 2);

    waiting_.dim = true;
    waiting_.size = 14.0f;
    waiting_.setText("Waiting for the device...");
    waiting_.setBounds(12, 34, 400, 24);
    synth_.addAndMakeVisible(waiting_);

    // ---- machine / filter machine selectors in the panel headers
    for (int m = 0; m < 5; ++m) machineBox_.addItem(ob::machineName(m), m + 1);
    for (int f = 0; f < 6; ++f) filterBox_.addItem(ob::filterMachineName(f), f + 1);
    machineBox_.setBounds(1018, 3, 118, 25);
    filterBox_.setBounds(491, 3, 118, 25);
    machineLabel_.size = 15.5f;
    machineLabel_.setText("Machine");
    machineLabel_.setBounds(940, 3, 74, 25);
    synth_.addAndMakeVisible(machineLabel_);
    synth_.addAndMakeVisible(machineBox_);
    filter_.addAndMakeVisible(filterBox_);
    machineBox_.onChange = [this] {
        const int m = machineBox_.getSelectedId() - 1;
        if (m >= 0 && m != proc_.model().machine(track_)) proc_.model().setMachine(track_, m);
    };
    filterBox_.onChange = [this] {
        const int f = filterBox_.getSelectedId() - 1;
        if (f >= 0 && f != proc_.model().filterMachine(track_)) proc_.model().setFilterMachine(track_, f);
    };

    // ---- BASE / WIDTH FILTER
    tint_ = &Tints::filter;
    {
        auto* grp = addGroup(baseWidth_, {}, {11, 38, 263, 191}, Tints::filter);
        addGraph(baseWidth_, Graph::Kind::BaseWidth, grp->getBounds().withHeight(128), Tints::filter);
        make(baseWidth_, paramIndex("Sound_FilterBase"), track_, centred(60, 168, 76, 60), Value, "Base");
        make(baseWidth_, paramIndex("Sound_FilterWidth"), track_, centred(141, 168, 76, 60), Value, "Width");
        auto lbl = std::make_unique<TextLine>();
        lbl->setText("Routing");
        lbl->size = 15.0f;
        lbl->justification = juce::Justification::centred;
        lbl->setBounds(centred(225, 168, 90, 22));
        baseWidth_.addAndMakeVisible(*lbl);
        extras_.push_back(std::move(lbl));
        make(baseWidth_, paramIndex("Sound_FilterBaseWidthRouting"), track_, {186, 192, 90, 32}, Radio);
    }

    // ---- FILTER: response (machine parameters in buildFilter) and envelope
    {
        auto* env = addGroup(filter_, {}, {313, 38, 295, 191}, Tints::filter);
        addGraph(filter_, Graph::Kind::FilterEnvelope, env->getBounds().withHeight(124).reduced(4, 0), Tints::filter);
        // as in Overbridge: Delay A Reset D Inf S R Inf
        auto label = [&](juce::Component& parent, const char* t, int cx) {
            auto l = std::make_unique<TextLine>();
            l->setText(t);
            l->size = 15.0f;
            l->justification = juce::Justification::centred;
            l->setBounds(centred(cx, 172, 50, 22));
            parent.addAndMakeVisible(*l);
            extras_.push_back(std::move(l));
        };
        make(filter_, paramIndex("Sound_FilterEnvDelay"), track_, centred(333, 172, 36, 60), Value, "Delay");
        make(filter_, paramIndex("Sound_FilterAttack"), track_, centred(370, 172, 36, 60), Value, "A");
        label(filter_, "Reset", 413);
        make(filter_, paramIndex("Sound_FilterEnvReset"), track_, centred(413, 196, 40, 22), Switch);
        make(filter_, paramIndex("Sound_FilterDecay"), track_, centred(458, 172, 38, 60), Value, "D");
        label(filter_, "Inf", 495);
        make(filter_, paramIndex("Sound_FilterDecay"), track_, centred(495, 196, 40, 22), Inf);
        make(filter_, paramIndex("Sound_FilterSustain"), track_, centred(527, 172, 36, 60), Value, "S");
        make(filter_, paramIndex("Sound_FilterRelease"), track_, centred(560, 172, 38, 60), Value, "R");
        label(filter_, "Inf", 594);
        make(filter_, paramIndex("Sound_FilterRelease"), track_, centred(594, 196, 40, 22), Inf);
    }

    // ---- AMP
    tint_ = &Tints::amp;
    {
        auto* grp = addGroup(amp_, {}, {9, 38, 314, 191}, Tints::amp);
        addGraph(amp_, Graph::Kind::AmpEnvelope, grp->getBounds().withHeight(128), Tints::amp);
        make(amp_, paramIndex("Sound_AmpMode"), track_, {grp->getX() + 120, grp->getY() + 8, 80, 22}, Hidden);
        // as in Overbridge: A Reset D Inf S R Inf
        auto label = [&](const char* t, int cx) {
            auto l = std::make_unique<TextLine>();
            l->setText(t);
            l->size = 15.0f;
            l->justification = juce::Justification::centred;
            l->setBounds(centred(cx, 172, 50, 22));
            amp_.addAndMakeVisible(*l);
            extras_.push_back(std::move(l));
        };
        make(amp_, paramIndex("Sound_AmpAttack"), track_, centred(32, 172, 40, 60), Value, "A");
        label("Reset", 73);
        make(amp_, paramIndex("Sound_AmpEnvReset"), track_, centred(73, 196, 40, 22), Switch);
        make(amp_, paramIndex("Sound_AmpDecay"), track_, centred(115, 172, 40, 60), Value, "D");
        label("Inf", 157);
        make(amp_, paramIndex("Sound_AmpDecay"), track_, centred(157, 196, 40, 22), Inf);
        make(amp_, paramIndex("Sound_AmpSustain"), track_, centred(198, 172, 40, 60), Value, "S");
        make(amp_, paramIndex("Sound_AmpRelease"), track_, centred(240, 172, 40, 60), Value, "R");
        label("Inf", 282);
        make(amp_, paramIndex("Sound_AmpRelease"), track_, centred(282, 196, 40, 22), Inf);
        tint_ = &Tints::amp;
        make(amp_, paramIndex("Sound_AmpPan"), track_, centred(370, 112, 76, 104), KnobK, "Pan");
        make(amp_, paramIndex("Sound_AmpVolume"), track_, centred(446, 112, 76, 104), KnobK, "Volume");
        make(amp_, paramIndex("Kit_TrackLevel"), track_, centred(522, 112, 96, 104), FaderK, "Track Level");
    }

    // ---- FX
    tint_ = &Tints::fx;
    {
        addGroup(fx_, {}, {10, 38, 443, 129}, Tints::fx);
        make(fx_, paramIndex("Sound_DistBitReduction"), track_, centred(58, 50, 80, 104), KnobK, "Bit Redux");
        make(fx_, paramIndex("Sound_DistSampleRateReduction"), track_, centred(143, 50, 80, 104), KnobK, "SRR");
        make(fx_, paramIndex("Sound_DistDistortion"), track_, centred(327, 50, 80, 104), KnobK, "Overdrive");
        for (int i = 0; i < 2; ++i) {
            auto lbl = std::make_unique<TextLine>();
            lbl->setText("Routing");
            lbl->size = 15.0f;
            lbl->justification = juce::Justification::centred;
            lbl->setBounds(centred(i == 0 ? 231 : 404, 51, 90, 22));
            fx_.addAndMakeVisible(*lbl);
            extras_.push_back(std::move(lbl));
            auto sep = std::make_unique<juce::Component>();
            extras_.push_back(std::move(sep));
        }
        make(fx_, paramIndex("Sound_DistSampleRateReductionRouting"), track_, {196, 95, 95, 32}, Radio);
        make(fx_, paramIndex("Sound_DistDistortionRouting"), track_, {369, 95, 95, 32}, Radio);
        auto* d = make(fx_, paramIndex("Sound_AmpDelaySend"), track_, centred(501, 50, 80, 104), Send, "Del. Send");
        auto* r = make(fx_, paramIndex("Sound_AmpReverbSend"), track_, centred(577, 50, 80, 104), Send, "Rev. Send");
        auto* c = make(fx_, paramIndex("Sound_AmpChorusSend"), track_, centred(653, 50, 84, 104), Send, "Chor. Send");
        if (d) static_cast<SendIcon*>(d)->icon = 0;
        if (r) static_cast<SendIcon*>(r)->icon = 1;
        if (c) static_cast<SendIcon*>(c)->icon = 2;
    }
    update();
}

void TrackPage::buildFmTone(juce::Component& panel)
{
    // Algorithm | Ratios + Ratio Offset / Key tracking | Envelope Operator A / B | Harmonics, Detune, Feedback, Phase reset
    auto group = [&](const juce::String& title, juce::Rectangle<int> b) {
        auto* g = addGroup(panel, title, b, Tints::synth);
        machineParts_.push_back(g);
        return g;
    };
    auto graph = [&](Graph::Kind k, juce::Rectangle<int> b) { machineParts_.push_back(addGraph(panel, k, b, Tints::synth)); };

    group("Algorithm", {11, 39, 212, 414});
    if (auto* c = static_cast<Choice*>(make(panel, paramIndex("Sound_FmAlgorithm"), track_, {19, 72, 196, 22}, ChoiceK)))
        c->options = {"1", "2", "3", "4", "5", "6", "7", "8"};
    addAlgorithmArrows(panel, "Sound_FmAlgorithm", 7, {20, 226, 22, 42}, {196, 226, 22, 42});
    graph(Graph::Kind::Algorithm, {50, 100, 124, 230});
    make(panel, paramIndex("Sound_FmMix"), track_, {35, 377, 164, 70}, HBar, "X-Y Mix");

    auto* ratios = group("Ratios", {232, 39, 340, 203});
    ratios->extraTitles = {"Ratio Offset"};
    make(panel, paramIndex("Sound_FmARatio"), track_, {249, 80, 70, 54}, Ratio, "A");
    // ratio B is one value holding the pair B2 / B1 (B2 above B1, joined by a line)
    if (auto* b2 = static_cast<ValueBox*>(make(panel, paramIndex("Sound_FmBRatio"), track_, {334, 80, 70, 54}, Ratio, "B2")))
        b2->ratioPart = 1;
    if (auto* b1 = static_cast<ValueBox*>(make(panel, paramIndex("Sound_FmBRatio"), track_, {334, 167, 70, 54}, Ratio, "B1")))
        b1->ratioPart = 2;
    {
        auto link = std::make_unique<juce::Component>();
        struct Line : juce::Component {
            void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(Tints::synth.accent).withAlpha(0.55f)); }
        };
        auto l = std::make_unique<Line>();
        l->setBounds(368, 134, 2, 33);
        l->setInterceptsMouseClicks(false, false);
        panel.addAndMakeVisible(*l);
        machineParts_.push_back(l.get());
        extras_.push_back(std::move(l));
    }
    make(panel, paramIndex("Sound_FmCRatio"), track_, {249, 167, 70, 54}, Ratio, "C");
    make(panel, paramIndex("Sound_FmARatioOffset"), track_, centred(458, 72, 70, 60), Value, "A");
    make(panel, paramIndex("Sound_FmB2RatioOffset"), track_, centred(525, 72, 70, 60), Value, "B2");
    make(panel, paramIndex("Sound_FmCRatioOffset"), track_, centred(458, 159, 70, 60), Value, "C");
    make(panel, paramIndex("Sound_FmB1RatioOffset"), track_, centred(525, 159, 70, 60), Value, "B1");

    group("Key tracking", {232, 250, 340, 203});
    make(panel, paramIndex("Sound_FmAKeyScale"), track_, centred(294, 284, 100, 166), VBar, "Operator A");
    make(panel, paramIndex("Sound_FmB1KeyScale"), track_, centred(400, 284, 100, 166), VBar, "Operator B1");
    make(panel, paramIndex("Sound_FmB2KeyScale"), track_, centred(507, 284, 100, 166), VBar, "Operator B2");

    const char* envTitles[] = {"Envelope Operator A", "Envelope Operator B"};
    for (int op = 0; op < 2; ++op) {
        const int y = op == 0 ? 39 : 250;
        group(envTitles[op], {579, y, 333, 203});
        graph(op == 0 ? Graph::Kind::FmEnvA : Graph::Kind::FmEnvB, {587, y + 34, 317, 104});
        const char* p = op == 0 ? "A" : "B";
        const int cx[] = {603, 649, 695, 742, 788, 835, 881};
        const juce::String keys[] = {juce::String("Sound_Fm") + p + "Delay", juce::String("Sound_Fm") + p + "Attack",
                                     juce::String("Sound_Fm") + p + "EnvReset", juce::String("Sound_Fm") + p + "Decay",
                                     juce::String("Sound_Fm") + p + "End", juce::String("Sound_Fm") + p + "Trig",
                                     juce::String("Sound_Fm") + p + "Level"};
        const char* labels[] = {"Delay", "Attack", "Reset", "Decay", "End", "Trig", "Level"};
        for (int i = 0; i < 7; ++i) {
            const int idx = paramIndex(keys[i].toRawUTF8());
            if (i == 2 || i == 5) {
                make(panel, idx, track_, centred(cx[i], y + 140, 46, 22).translated(0, 24), Switch);
                auto lbl = std::make_unique<TextLine>();
                lbl->setText(labels[i]);
                lbl->size = 15.0f;
                lbl->justification = juce::Justification::centred;
                lbl->setBounds(centred(cx[i], y + 140, 50, 22));
                panel.addAndMakeVisible(*lbl);
                machineParts_.push_back(lbl.get());
                extras_.push_back(std::move(lbl));
            } else
                make(panel, idx, track_, centred(cx[i], y + 140, 46, 60), Value, labels[i]);
        }
    }

    group({}, {919, 39, 218, 133});
    graph(Graph::Kind::Harmonics, {929, 47, 198, 58});
    make(panel, paramIndex("Sound_FmHarmonics"), track_, centred(1028, 108, 110, 60), Value, "Harmonics");
    group({}, {919, 180, 105, 132});
    graph(Graph::Kind::Detune, {924, 188, 95, 52});
    make(panel, paramIndex("Sound_FmDetune"), track_, centred(971, 247, 100, 60), Value, "Detune");
    group({}, {1032, 180, 105, 132});
    graph(Graph::Kind::Feedback, {1037, 186, 95, 56});
    make(panel, paramIndex("Sound_FmFeedback"), track_, centred(1084, 247, 100, 60), Value, "Feedback");
    group("Phase reset", {919, 320, 218, 133});
    if (auto* c = static_cast<Choice*>(make(panel, paramIndex("Sound_FmPhaseReset"), track_, {929, 360, 198, 64}, ChoiceK))) {
        c->options = {"OFF", "ALL", "C", "A+B", "A+B2"};
        c->firstInRow = 1;
        c->bigValue = true;
    }
}

void TrackPage::paramGroup(juce::Component& panel, const juce::String& title, juce::Rectangle<int> box,
                           std::initializer_list<std::pair<const char*, const char*>> params, int columns)
{
    machineParts_.push_back(addGroup(panel, title, box, Tints::synth));
    const int rows = ((int)params.size() + columns - 1) / columns;
    const int w = (box.getWidth() - 10) / columns;
    const int top = box.getY() + (title.isEmpty() ? 12 : 36);
    const int h = std::min(84, (box.getBottom() - top - 6) / std::max(1, rows));
    int i = 0;
    for (auto& [key, label] : params) {
        const int idx = paramIndex(key);
        const auto cell = juce::Rectangle<int>(box.getX() + 5 + (i % columns) * w, top + (i / columns) * h, w, h);
        if (idx >= 0) {
            const auto& d = ob::digitone2Params()[(size_t)idx];
            if (d.format == ob::ParamFormat::OnOff) {
                make(panel, idx, track_, cell.withSizeKeepingCentre(w, 22).translated(0, 12), Switch);
                auto lbl = std::make_unique<TextLine>();
                lbl->setText(label);
                lbl->size = 15.0f;
                lbl->justification = juce::Justification::centred;
                lbl->setBounds(cell.withHeight(22).translated(0, -2));
                panel.addAndMakeVisible(*lbl);
                machineParts_.push_back(lbl.get());
                extras_.push_back(std::move(lbl));
            } else
                make(panel, idx, track_, cell.withSizeKeepingCentre(std::min(w, 110), 60), Value, label);
        }
        ++i;
    }
}

Group* TrackPage::machineGroup(juce::Component& panel, const juce::String& title, juce::Rectangle<int> b)
{
    auto* g = addGroup(panel, title, b, Tints::synth);
    machineParts_.push_back(g);
    return g;
}

void TrackPage::machineGraph(juce::Component& panel, Graph::Kind kind, std::vector<std::string> keys, juce::Rectangle<int> b)
{
    auto gph = std::make_unique<Graph>(proc_, track_, kind, std::move(keys));
    gph->tint = &Tints::synth;
    gph->fill = false;
    gph->setBounds(b);
    panel.addAndMakeVisible(*gph);
    machineParts_.push_back(gph.get());
    graphs_.push_back(std::move(gph));
}

void TrackPage::machineLabel(juce::Component& panel, const juce::String& text, juce::Rectangle<int> b, float size, juce::uint32 colour)
{
    auto lbl = std::make_unique<TextLine>();
    lbl->setText(text);
    lbl->size = size;
    lbl->colour = colour;
    lbl->justification = juce::Justification::centred;
    lbl->setBounds(b);
    panel.addAndMakeVisible(*lbl);
    machineParts_.push_back(lbl.get());
    extras_.push_back(std::move(lbl));
}

void TrackPage::addAlgorithmArrows(juce::Component& panel, const char* key, int maxIndex, juce::Rectangle<int> left,
                                   juce::Rectangle<int> right)
{
    // previous / next algorithm: thin chevrons
    struct Chevron : juce::Button {
        explicit Chevron(bool l) : juce::Button("algorithm"), left_(l) {}
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced(4, 6);
            juce::Path p;
            p.startNewSubPath(left_ ? r.getRight() : r.getX(), r.getY());
            p.lineTo(left_ ? r.getX() : r.getRight(), r.getCentreY());
            p.lineTo(left_ ? r.getRight() : r.getX(), r.getBottom());
            g.setColour(juce::Colour(Tints::synth.title).brighter(over ? 0.3f : 0.0f));
            g.strokePath(p, juce::PathStrokeType(2.2f));
        }
        bool left_;
    };
    for (int dir = -1; dir <= 1; dir += 2) {
        auto b = std::make_unique<Chevron>(dir < 0);
        b->setBounds(dir < 0 ? left : right);
        const std::string k = key;
        b->onClick = [this, dir, k, maxIndex] {
            const int i = paramIndex(k.c_str());
            const int v = juce::jlimit(0, maxIndex, (proc_.model().get(i, track_) >> 8) + dir);
            proc_.editorGesture = true;
            if (auto* p = proc_.parameterFor(i, track_)) p->setValueNotifyingHost(ob::normalisedFromRaw(ob::digitone2Params()[(size_t)i], v << 8));
            proc_.editorGesture = false;
        };
        panel.addAndMakeVisible(*b);
        machineParts_.push_back(b.get());
        extras_.push_back(std::move(b));
    }
}

void TrackPage::buildFmDrum(juce::Component& panel)
{
    // FM Drum page (page coordinates, panel at x 3)
    auto X = [](int x) { return x - 3; };
    auto grp = [&](int x0, int y0, int x1, int y1, const juce::String& title = {}) {
        machineGroup(panel, title, {X(x0), y0, x1 - x0, y1 - y0});
    };
    auto val = [&](const char* key, const char* label, int cx, int labelY, int w = 84) {
        make(panel, paramIndex(key), track_, centred(X(cx), labelY - 11, w, 60), Value, label);
    };
    auto gph = [&](Graph::Kind k, std::vector<std::string> keys, int x0, int y0, int x1, int y1) {
        machineGraph(panel, k, std::move(keys), {X(x0), y0, x1 - x0, y1 - y0});
    };
    gph(Graph::Kind::SectionBar, {"Body"}, 15, 47, 718, 70);
    gph(Graph::Kind::SectionBar, {"Noise & Transient"}, 726, 47, 1043, 70);
    gph(Graph::Kind::SectionBar, {"Mix"}, 1052, 47, 1138, 70);

    grp(15, 76, 200, 222, "Algorithm");
    if (auto* c = static_cast<Choice*>(make(panel, paramIndex("Sound_DrumAlgo"), track_, {X(25), 106, 170, 20}, ChoiceK)))
        c->options = {"1", "2", "3", "4", "5", "6", "7"};
    machineParts_.push_back(addGraph(panel, Graph::Kind::DrumAlgorithm, {X(48), 116, 120, 100}, Tints::synth));
    addAlgorithmArrows(panel, "Sound_DrumAlgo", 6, {X(24), 150, 20, 36}, {X(172), 150, 20, 36});
    grp(15, 230, 200, 462);
    gph(Graph::Kind::Waveform, {"Sound_DrumWave1"}, 25, 238, 100, 280);
    gph(Graph::Kind::Waveform, {"Sound_DrumWave2"}, 118, 238, 193, 280);
    val("Sound_DrumWave1", "C Wave", 57, 297);
    val("Sound_DrumWave2", "A B Wave", 150, 297);
    gph(Graph::Kind::FeedbackIcon, {}, 40, 355, 90, 395);
    gph(Graph::Kind::FoldIcon, {"Sound_DrumFold"}, 115, 360, 190, 390);
    val("Sound_DrumFeedback", "Feedback", 57, 412);
    val("Sound_DrumFold", "Wavefold", 150, 412);

    grp(207, 76, 310, 198);
    gph(Graph::Kind::Ruler, {"Sound_DrumTune"}, 218, 86, 298, 130);
    val("Sound_DrumTune", "Tune", 258, 157);
    grp(315, 76, 413, 198);
    gph(Graph::Kind::PhaseStart, {"Sound_DrumPhaseWave1"}, 328, 79, 394, 145);
    val("Sound_DrumPhaseWave1", "C Phase Start", 365, 157, 96);
    grp(207, 206, 413, 272);
    val("Sound_DrumSweep", "Sweep Time", 258, 232);
    val("Sound_DrumSweepDepth", "Sweep Depth", 365, 232);
    grp(207, 280, 413, 462, "Body Envelope");
    gph(Graph::Kind::SimpleEnvelope, {"", "Sound_DrumBodyHold", "Sound_DrumBodyDecay"}, 222, 318, 408, 382);
    val("Sound_DrumBodyHold", "Hold", 258, 412);
    val("Sound_DrumBodyDecay", "Decay", 365, 412);

    grp(420, 76, 718, 462, "Ratios & Modulation Envelopes");
    gph(Graph::Kind::OpEnvelope, {"Sound_DrumDecay1", "Sound_DrumEnd1", "Sound_DrumMod1"}, 437, 112, 690, 205);
    const int cx[] = {470, 548, 627, 690};
    const char* ka[] = {"Sound_DrumRatio1", "Sound_DrumDecay1", "Sound_DrumEnd1", "Sound_DrumMod1"};
    const char* la[] = {"Ratio A", "Decay A", "End A", "Mod A"};
    const char* kb[] = {"Sound_DrumRatio2", "Sound_DrumDecay2", "Sound_DrumEnd2", "Sound_DrumMod2"};
    const char* lb[] = {"Ratio B", "Decay B", "End B", "Mod B"};
    for (int i = 0; i < 4; ++i) val(ka[i], la[i], cx[i], 232, 70);
    gph(Graph::Kind::OpEnvelope, {"Sound_DrumDecay2", "Sound_DrumEnd2", "Sound_DrumMod2"}, 437, 292, 690, 385);
    for (int i = 0; i < 4; ++i) val(kb[i], lb[i], cx[i], 412, 70);

    grp(726, 76, 835, 462);
    make(panel, paramIndex("Sound_DrumTransient"), track_, centred(X(778), 86, 84, 100), KnobK, "Transient");
    make(panel, paramIndex("Sound_DrumNoiseGrain"), track_, centred(X(778), 221, 84, 100), KnobK, "Noise Grain");
    machineLabel(panel, "Noise Reset", centred(X(778), 349, 100, 22));
    make(panel, paramIndex("Sound_DrumNoiseReset"), track_, centred(X(778), 370, 46, 22), Switch);
    machineLabel(panel, "N. Ring Mod", centred(X(778), 401, 100, 22));
    make(panel, paramIndex("Sound_DrumNoiseRingMod"), track_, centred(X(778), 422, 46, 22), Switch);
    grp(838, 76, 1040, 285, "Noise Base / Width Filter");
    gph(Graph::Kind::BandPass, {"Sound_DrumNoiseBase", "Sound_DrumNoiseWidth"}, 852, 118, 1028, 205);
    val("Sound_DrumNoiseBase", "Base", 893, 232);
    val("Sound_DrumNoiseWidth", "Width", 987, 232);
    grp(838, 290, 1040, 462, "Noise Envelope");
    gph(Graph::Kind::SimpleEnvelope, {"", "Sound_DrumNoiseHold", "Sound_DrumNoiseDecay"}, 852, 328, 1028, 385);
    val("Sound_DrumNoiseHold", "Hold", 893, 412);
    val("Sound_DrumNoiseDecay", "Decay", 987, 412);

    grp(1052, 76, 1138, 462);
    make(panel, paramIndex("Sound_DrumBodyLevel"), track_, {X(1062), 86, 66, 118}, FaderK, "Body");
    make(panel, paramIndex("Sound_DrumTransientLevel"), track_, {X(1062), 214, 66, 118}, FaderK, "Transient");
    make(panel, paramIndex("Sound_DrumNoiseLevel"), track_, {X(1062), 342, 66, 118}, FaderK, "Noise");
}

void TrackPage::buildWaveTone(juce::Component& panel)
{
    // WaveTone page (page coordinates, panel at x 3)
    auto X = [](int x) { return x - 3; };
    auto grp = [&](int x0, int y0, int x1, int y1, const juce::String& title = {}) {
        machineGroup(panel, title, {X(x0), y0, x1 - x0, y1 - y0});
    };
    auto val = [&](const std::string& key, const char* label, int cx, int labelY, int w = 84) {
        make(panel, paramIndex(key.c_str()), track_, centred(X(cx), labelY - 11, w, 60), Value, label);
    };
    auto gph = [&](Graph::Kind k, std::vector<std::string> keys, int x0, int y0, int x1, int y1) {
        machineGraph(panel, k, std::move(keys), {X(x0), y0, x1 - x0, y1 - y0});
    };
    for (int o = 1; o <= 2; ++o) {
        const int dy = o == 1 ? 0 : 210;
        const std::string n = std::to_string(o), k = "Sound_VaOsc" + n;
        grp(15, 47 + dy, 515, 250 + dy, "Oscillator " + juce::String(o));
        gph(Graph::Kind::Ruler, {k + "Tune"}, 27, 102 + dy, 105, 158 + dy);
        gph(Graph::Kind::CrossIcon, {k + "LinearOffset"}, 122, 100 + dy, 182, 160 + dy);
        gph(Graph::Kind::TableIcon, {k + "WaveTable"}, 209, 100 + dy, 269, 160 + dy);
        gph(Graph::Kind::Waveform, {k + "Waveform", k + "PhaseDist"}, 297, 87 + dy, 434, 177 + dy);
        if (auto* f = static_cast<Fader*>(make(panel, paramIndex((k + "Level").c_str()), track_, {X(471), 98 + dy, 16, 64}, FaderK, "-")))
            f->showValue = false;
        val(k + "Tune", "Tune", 66, 205 + dy);
        val(k + "LinearOffset", "Linear Offset", 152, 205 + dy, 100);
        val(k + "WaveTable", "Table", 239, 205 + dy);
        val(k + "Waveform", "Wave", 331, 205 + dy);
        val(k + "PhaseDist", "Phase Dist.", 406, 205 + dy, 96);
        val(k + "Level", "Level", 480, 205 + dy, 60);
    }
    grp(525, 47, 731, 179, "Oscillator Drift");
    gph(Graph::Kind::DriftIcon, {"Sound_VaOscDrift"}, 595, 80, 668, 140);
    make(panel, paramIndex("Sound_VaOscDrift"), track_, centred(X(631), 142, 84, 30), Value, {});
    if (auto* v = dynamic_cast<ValueBox*>(controls_.back().get())) v->showLabel = false;
    grp(525, 186, 731, 375, "Oscillator Mod");
    machineParts_.push_back(addGraph(panel, Graph::Kind::OscMod, {X(545), 225, 170, 76}, Tints::synth));
    if (auto* c = static_cast<Choice*>(make(panel, paramIndex("Sound_VaOscMod"), track_, {X(532), 324, 196, 38}, ChoiceK))) {
        c->options = {"OFF", "Ring Mod", "Ring Mod (Fixed)", "Hard Sync"};
        c->columns = 2;
        c->order = {0, 1, 3, 2}; // OFF, Ring Mod / Hard Sync, Ring Mod (Fixed)
    }
    grp(525, 384, 731, 460, "Oscillator Phase Reset");
    if (auto* c = static_cast<Choice*>(make(panel, paramIndex("Sound_VaOscPhaseReset"), track_, {X(540), 420, 180, 24}, ChoiceK)))
        c->options = {"OFF", "ON", "RANDOM"};
    grp(740, 47, 835, 460, "Noise Type");
    if (auto* c = static_cast<Choice*>(make(panel, paramIndex("Sound_VaNoiseType"), track_, {X(745), 91, 84, 60}, ChoiceK))) {
        c->options = {"GRAIN", "TUNED", "S&H"};
        c->vertical = true;
    }
    make(panel, paramIndex("Sound_VaNoiseCharacter"), track_, centred(X(787), 193, 84, 98), KnobK, "Character");
    if (auto* f = static_cast<Fader*>(make(panel, paramIndex("Sound_VaNoiseLevel"), track_, {X(778), 328, 18, 70}, FaderK, "-")))
        f->showValue = false;
    val("Sound_VaNoiseLevel", "Level", 787, 416, 60);
    grp(844, 47, 1137, 250, "Noise Base / Width Filter");
    gph(Graph::Kind::BandPass, {"Sound_VaNoiseBase", "Sound_VaNoiseWidth"}, 855, 97, 1127, 176);
    val("Sound_VaNoiseBase", "Base", 922, 205);
    val("Sound_VaNoiseWidth", "Width", 1056, 205);
    grp(844, 257, 1137, 460, "Noise Envelope");
    gph(Graph::Kind::SimpleEnvelope, {"Sound_VaNoiseAttack", "Sound_VaNoiseHold", "Sound_VaNoiseDecay"}, 862, 312, 1131, 384);
    val("Sound_VaNoiseAttack", "Attack", 880, 416, 60);
    val("Sound_VaNoiseHold", "Hold", 935, 416, 60);
    machineLabel(panel, "Note", centred(X(989), 405, 50, 22));
    make(panel, paramIndex("Sound_VaNoiseHold"), track_, centred(X(989), 427, 40, 22), Inf);
    val("Sound_VaNoiseDecay", "Decay", 1043, 416, 60);
    machineLabel(panel, "Inf", centred(X(1095), 405, 50, 22));
    make(panel, paramIndex("Sound_VaNoiseDecay"), track_, centred(X(1095), 427, 40, 22), Inf);
}

void TrackPage::buildSwarmer(juce::Component& panel)
{
    // Swarmer page (page coordinates, panel at x 3)
    auto X = [](int x) { return x - 3; };
    auto grp = [&](int x0, int y0, int x1, int y1, const juce::String& title = {}) {
        machineGroup(panel, title, {X(x0), y0, x1 - x0, y1 - y0});
    };
    auto val = [&](const char* key, const char* label, int cx, int labelY, int w = 84) {
        make(panel, paramIndex(key), track_, centred(X(cx), labelY - 11, w, 60), Value, label);
    };
    auto gph = [&](Graph::Kind k, std::vector<std::string> keys, int x0, int y0, int x1, int y1) {
        machineGraph(panel, k, std::move(keys), {X(x0), y0, x1 - x0, y1 - y0});
    };
    grp(10, 47, 190, 250);
    gph(Graph::Kind::Ruler, {"Sound_SwarmerTune"}, 50, 100, 150, 160);
    val("Sound_SwarmerTune", "Tune", 100, 194);
    grp(198, 47, 378, 250, "Swarm Oscillator");
    gph(Graph::Kind::Waveform, {"Sound_SwarmerWaveSwarm"}, 235, 95, 345, 163);
    val("Sound_SwarmerWaveSwarm", "Waveform", 287, 194);
    grp(10, 256, 378, 460, "Main Oscillator");
    if (auto* c = static_cast<Choice*>(make(panel, paramIndex("Sound_SwarmerSubOctave"), track_, {X(82), 306, 50, 70}, ChoiceK))) {
        c->options = {"-2", "-1", "0"}; // raw 0..2 is shown -2..0
        c->vertical = true;
        c->order = {2, 1, 0};
    }
    val("Sound_SwarmerSubOctave", "Octave", 100, 403);
    gph(Graph::Kind::Waveform, {"Sound_SwarmerWaveMain"}, 235, 304, 345, 374);
    val("Sound_SwarmerWaveMain", "Waveform", 287, 403);
    grp(385, 47, 1134, 460, "Swarm Setup and Animation");
    gph(Graph::Kind::AnimIcon, {"Sound_SwarmerAnimation"}, 431, 210, 509, 281);
    val("Sound_SwarmerAnimation", "Swarm Animation", 469, 321, 110);
    gph(Graph::Kind::SwarmLines, {"Sound_SwarmerDetune", "Sound_SwarmerMix"}, 560, 140, 960, 343);
    val("Sound_SwarmerDetune", "Swarm Detune", 671, 403, 110);
    val("Sound_SwarmerMix", "Swarm Mix", 850, 403, 110);
    gph(Graph::Kind::NoiseDots, {"Sound_SwarmerNoiseMod"}, 994, 231, 1104, 266);
    val("Sound_SwarmerNoiseMod", "Noise Modulation", 1048, 321, 120);
}

void TrackPage::buildMidi(juce::Component& panel)
{
    // MIDI machine: only a message
    machineGroup(panel, {}, {11, 39, 1126, 414});
    machineLabel(panel, "MIDI settings are available on the device", {11, 200, 1126, 30}, 20.0f, Tints::synth.title);
}

void TrackPage::buildMachine()
{
    removeControls(machineControls_);
    removeOwned(machineParts_);
    const int m = proc_.model().machine(track_);
    machineBox_.setSelectedId(m + 1, juce::dontSendNotification);
    if (m < 0) return;
    tint_ = &Tints::synth;
    const size_t before = controls_.size();
    if (m == 0) buildFmTone(synth_);
    else if (m == 1) buildWaveTone(synth_);
    else if (m == 2) buildFmDrum(synth_);
    else if (m == 3) buildSwarmer(synth_);
    else if (m == 4) buildMidi(synth_);
    else {
        // other machines: one group with the machine's parameters
        auto* g = addGroup(synth_, ob::machineName(m), {11, 39, 1126, 414}, Tints::synth);
        machineParts_.push_back(g);
        const auto params = paramsWhere([m](const ob::ParamDef& d) { return ob::machineOf(d) == m; });
        place(synth_, {21, 74, 1106, 370}, params, 8, 84);
    }
    for (size_t i = before; i < controls_.size(); ++i) machineControls_.push_back(controls_[i].get());
}

void TrackPage::buildFilter()
{
    removeControls(filterControls_);
    removeOwned(filterParts_);
    const int f = proc_.model().filterMachine(track_);
    filterBox_.setSelectedId(f + 1, juce::dontSendNotification);
    tint_ = &Tints::filter;
    auto* grp = addGroup(filter_, {}, {10, 38, 295, 191}, Tints::filter);
    filterParts_.push_back(grp);
    filterParts_.push_back(addGraph(filter_, Graph::Kind::FilterResponse, grp->getBounds().withHeight(124).withWidth(212), Tints::filter));
    const size_t before = controls_.size();
    // as in Overbridge: the machine's mode / type / Q / LPF as a knob above (Multi-Mode: "Filter Type"),
    // the other parameters with Env. Depth and Key Tr. in the row below
    std::vector<std::pair<int, juce::String>> items;
    if (f >= 0) {
        auto params = paramsWhere([f](const ob::ParamDef& d) { return ob::filterMachineOf(d) == f; });
        std::stable_sort(params.begin(), params.end(), [](int a, int b) {
            return ob::digitone2Params()[(size_t)a].knob < ob::digitone2Params()[(size_t)b].knob;
        });
        for (int p : params) {
            const auto& d = ob::digitone2Params()[(size_t)p];
            if (d.knob == 66) make(filter_, p, track_, centred(258, 74, 80, 98), KnobK, d.name);
            else items.push_back({p, d.name});
        }
    }
    items.push_back({paramIndex("Sound_FilterEnvDepth"), "Env. Depth"});
    items.push_back({paramIndex("Sound_FilterKeytrack"), "Key Tr."});
    const int n = (int)items.size();
    const int cx4[] = {48, 128, 198, 268};
    for (int i = 0; i < n; ++i) {
        const int cx = n == 4 ? cx4[i] : 10 + (2 * i + 1) * 295 / (2 * n);
        make(filter_, items[(size_t)i].first, track_, centred(cx, 172, 78, 60), Value, items[(size_t)i].second);
    }
    for (size_t i = before; i < controls_.size(); ++i) filterControls_.push_back(controls_[i].get());
}

void TrackPage::buildLfo()
{
    removeControls(lfoControls_);
    removeOwned(lfoParts_);
    const int tabSel = juce::jlimit(0, 8, proc_.uiLfo);
    builtLfo_ = tabSel;
    const int tabX[] = {134, 184, 232, 282, 348, 441, 521, 617, 683}, tabW[] = {40, 35, 35, 51, 80, 66, 81, 54, 94};
    for (int i = 0; i < 9; ++i) lfoTabs_[(size_t)i]->setBounds(tabX[i] - 4, 3, tabW[i] + 8, Panel::kHeader - 2);
    tint_ = &Tints::mod;
    if (tabSel >= 3) {
        buildModSource(tabSel - 3);
        return;
    }
    const int lfo = tabSel;
    const juce::String n(lfo + 1);
    auto key = [&](const char* k) { return paramIndex((juce::String("Sound_Lfo") + n + k).toRawUTF8()); };
    const size_t before = controls_.size();
    auto* g = addGroup(modulation_, {}, {11, 38, 212, 129}, Tints::mod);
    lfoParts_.push_back(g);
    lfoParts_.push_back(addGraph(modulation_, Graph::Kind::Lfo, {16, 66, 202, 72}, Tints::mod, lfo));
    if (auto* c = static_cast<Choice*>(make(modulation_, key("Waveform"), track_, {15, 42, 204, 26}, ChoiceK))) {
        c->options = {"TRI", "SINE", "SQR", "SAW", "EXP", "RMP", "RND"};
        c->waveIcons = true;
    }
    if (auto* c = static_cast<Choice*>(make(modulation_, key("Trig"), track_, {15, 140, 204, 24}, ChoiceK)))
        c->options = {"FREE", "TRIG", "HOLD", "ONE", "HALF"};
    if (auto* box = static_cast<ChoiceBox*>(make(modulation_, key("Destination"), track_, {237, 48, 207, 72}, ChoiceBoxK))) {
        // menu index -> target slot -> parameter name for the track's machines
        juce::StringArray names;
        std::vector<int> values;
        const int m = proc_.model().machine(track_), f = proc_.model().filterMachine(track_);
        for (int idx = 0; idx < 128; ++idx) {
            const int slot = ob::DeviceModel::lfoDestinationSlot(lfo, idx);
            if (slot < 0) continue;
            const juce::String name = destinationName(slot, m, f);
            if (name.isEmpty()) continue;
            names.add(name);
            values.push_back(idx << 8);
        }
        box->setOptions(names, values);
    }
    const char* kn[] = {"Depth", "Start", "Fade", "Speed", "Mult"};
    const char* kl[] = {"Depth", "Start Phase", "Fade", "Speed", "Multiplier"};
    const int cx[] = {474, 539, 604, 670, 735};
    for (int i = 0; i < 5; ++i) make(modulation_, key(kn[i]), track_, centred(cx[i], 46, 74, 108), KnobK, kl[i]);
    for (size_t i = before; i < controls_.size(); ++i) lfoControls_.push_back(controls_[i].get());
}

juce::String TrackPage::destinationName(int slot, int machine, int filter)
{
    if (slot == 0) return "None";
    for (auto& d : ob::digitone2Params()) {
        if (d.object != ob::ParamObject::Sound || d.slot != slot) continue;
        if (ob::machineOf(d) >= 0 && ob::machineOf(d) != machine) continue;
        if (ob::filterMachineOf(d) >= 0 && ob::filterMachineOf(d) != filter) continue;
        return juce::String(ob::machineOf(d) >= 0 ? ob::machineName(machine) : d.sectionName) + " " + d.name;
    }
    return {};
}

void TrackPage::buildModSource(int source)
{
    // KEY TR. ... AFTERTOUCH: four destinations with a depth each (sound setup @0x35)
    auto& model = proc_.model();
    const int m = model.machine(track_), f = model.filterMachine(track_);
    std::vector<std::pair<int, int>> order; // menu index, slot
    for (int slot = 0; slot < 107; ++slot) {
        const int idx = ob::DeviceModel::modDestinationIndex(slot);
        if (idx >= 0 && (slot == 0 || destinationName(slot, m, f).isNotEmpty())) order.push_back({idx, slot});
    }
    std::sort(order.begin(), order.end());
    juce::StringArray names;
    std::vector<int> values;
    for (auto& [idx, slot] : order) {
        names.add(destinationName(slot, m, f));
        values.push_back(slot);
    }
    const int t = track_;
    const char* letters[] = {"A", "B", "C", "D"};
    for (int e = 0; e < 4; ++e) {
        const int x = 11 + e * 190;
        auto* g = addGroup(modulation_, letters[e], {x, 38, 182, 129}, Tints::mod);
        lfoParts_.push_back(g);
        auto combo = std::make_unique<ValueCombo>(Getter([&model, t, source, e] { return model.modValue(t, source, e, true); }),
                                                  Setter([&model, t, source, e](int v) { model.setModValue(t, source, e, true, v); }),
                                                  names, values);
        combo->setBounds(x + 8, 66, 166, 28);
        modulation_.addAndMakeVisible(*combo);
        lfoParts_.push_back(combo.get());
        extras_.push_back(std::move(combo));
        auto depth = std::make_unique<SignedValue>(Getter([&model, t, source, e] { return model.modValue(t, source, e, false); }),
                                                   Setter([&model, t, source, e](int v) { model.setModValue(t, source, e, false, v); }),
                                                   Tints::mod, "Depth");
        depth->setBounds(x + 41, 102, 100, 60);
        modulation_.addAndMakeVisible(*depth);
        lfoParts_.push_back(depth.get());
        extras_.push_back(std::move(depth));
    }
}

void TrackPage::buildSetup()
{
    removeControls(setupControls_);
    removeOwned(setupParts_);
    const int tab = juce::jlimit(0, 3, proc_.uiSetupTab);
    builtSetup_ = tab;
    tint_ = &Tints::plain;
    const size_t before = controls_.size();
    auto text = [&](const juce::String& t, juce::Rectangle<int> b, bool dim) {
        auto l = std::make_unique<TextLine>();
        l->setText(t);
        l->size = 15.0f;
        l->dim = dim;
        l->justification = juce::Justification::centred;
        l->setBounds(b);
        setup_.addAndMakeVisible(*l);
        setupParts_.push_back(l.get());
        extras_.push_back(std::move(l));
    };
    using F = ob::DeviceModel;
    auto& model = proc_.model();
    const int t = track_;
    auto own = [&](std::unique_ptr<juce::Component> c, juce::Rectangle<int> b) {
        c->setBounds(b);
        setup_.addAndMakeVisible(*c);
        setupParts_.push_back(c.get());
        extras_.push_back(std::move(c));
    };
    auto field = [&model, t](F::SetupField f) {
        return std::make_pair(Getter([&model, t, f] { return model.setupField(t, f); }),
                              Setter([&model, t, f](int v) { model.setSetupField(t, f, v); }));
    };
    auto voice = [&model, t](F::Voice f) {
        return std::make_pair(Getter([&model, t, f] { return model.voiceField(t, f); }),
                              Setter([&model, t, f](int v) { model.setVoiceField(t, f, v); }));
    };
    auto combo = [&](std::pair<Getter, Setter> gs, const juce::StringArray& opts, juce::Rectangle<int> b, int first = 0) {
        own(std::make_unique<SetupCombo>(gs.first, gs.second, opts, first), b);
    };
    auto bar = [&](std::pair<Getter, Setter> gs, int lo, int hi, int step, juce::Rectangle<int> b) {
        own(std::make_unique<SetupBar>(gs.first, gs.second, lo, hi, step), b);
    };
    if (tab == 0) {
        // TRK
        text("Pitch Bend Depth", centred(93, 56, 160, 22), false);
        bar(field(F::PitchBendDepth), 0, 60, 1, {34, 86, 130, 24});
        text("Octave offset", centred(242, 56, 160, 22), false);
        bar(field(F::Transpose), -60, 60, 12, {183, 86, 130, 24});
        text("Play Mode", centred(93, 130, 160, 22), false);
        combo(field(F::PlayMode), {"Poly", "Poly (M. LFO)", "Mono", "Mono (Leg)"}, {30, 156, 127, 29});
        text("Mono Note Prio", centred(242, 130, 160, 22), false);
        combo(field(F::MonoNotePriority), {"Last", "Low", "High"}, {179, 156, 127, 29});
        text("Velocity to Volume", centred(93, 214, 160, 22), false);
        combo(field(F::VelocityToVolume), {"Off", "Logarithm", "Linear", "Exponential", "Exponential 2"}, {30, 240, 127, 29});
    } else if (tab == 1) {
        // VOICE: kit voice configuration
        juce::StringArray amounts;
        for (int i = 2; i <= 16; ++i) amounts.add(juce::String(i));
        juce::StringArray locked;
        for (int i = 0; i <= 16; ++i) locked.add(juce::String(i));
        text("Unison", centred(93, 46, 160, 22), false);
        combo(voice(F::Voice::Unison), {"Off", "On"}, {30, 70, 127, 29});
        text("Voice Amount", centred(242, 46, 160, 22), false);
        combo(voice(F::Voice::VoiceAmount), amounts, {179, 70, 127, 29}, 2);
        text("Spread", centred(93, 112, 160, 22), false);
        bar(voice(F::Voice::Spread), 0, 127, 1, {34, 140, 130, 24});
        text("Track Locked Voices", centred(242, 112, 160, 22), false);
        combo(voice(F::Voice::LockedVoices), locked, {179, 136, 127, 29});
        text("Reuse voice", centred(93, 178, 160, 22), false);
        combo(voice(F::Voice::Reuse), {"Off", "On"}, {30, 202, 127, 29});
        text("Voice Stealing Behaviour (Global)", {10, 238, 318, 22}, false);
        combo({Getter([&model] { return model.voiceSteal(); }), Setter([&model](int v) { model.setVoiceSteal(v); })},
              {"Cycle", "Track", "Low", "High"}, {105, 262, 127, 29});
        if (!model.voiceConfigLoaded()) text("The device did not send its voice configuration", {10, 32, 318, 16}, true);
    } else if (tab == 2) {
        make(setup_, paramIndex("Sound_PortamentoTime"), track_, centred(93, 50, 120, 60), Value, "Time");
        make(setup_, paramIndex("Sound_PortamentoEnabled"), track_, {190, 72, 130, 22}, Dot, "Portamento");
        text("Type", centred(93, 122, 160, 22), false);
        combo(field(F::PortamentoType), {"Track", "Voice", "Legato Only"}, {30, 146, 127, 29});
        text("Style", centred(242, 122, 160, 22), false);
        combo(field(F::PortamentoStyle), {"Glide", "Glissando"}, {179, 146, 127, 29});
        text("Slope", centred(93, 200, 160, 22), false);
        combo(field(F::PortamentoSlope), {"Constant rate", "Constant time"}, {30, 224, 127, 29});
    } else {
        // LAYER: tracks triggered together with this one ("Track Trig Mask")
        text("Track Trig Mask", centred(169, 40, 300, 22), false);
        own(std::make_unique<TrigMaskGrid>(model, track_), {20, 66, 298, 218});
        if (!model.kitSetupLoaded()) text("The device did not send its kit setup", {10, 282, 318, 16}, true);
    }
    for (size_t i = before; i < controls_.size(); ++i) setupControls_.push_back(controls_[i].get());
}

void TrackPage::update()
{
    if (!built_) return;
    if (proc_.model().machine(track_) != builtMachine_) {
        builtMachine_ = proc_.model().machine(track_);
        buildMachine();
        builtLfo_ = -1; // destination names depend on the machine
    }
    if (proc_.model().filterMachine(track_) != builtFilter_) {
        builtFilter_ = proc_.model().filterMachine(track_);
        buildFilter();
        builtLfo_ = -1;
    }
    if (builtLfo_ < 0) buildLfo();
    if (builtSetup_ < 0) buildSetup();
    const juce::String name = proc_.model().soundName(track_);
    soundName_.setText(name.isEmpty() ? juce::String("-") : name);
    {
        const auto tags = ob::presetTagList(proc_.model().soundTags(track_));
        soundTags_.setText(tags.empty() ? juce::String("No tags.") : juce::String(tags));
    }
    waiting_.setVisible(!proc_.model().trackLoaded(track_));
}

void TrackPage::showSaveMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader("Save to the +Drive");
    menu.addItem(1, "Not available yet: the Digitone II ignores the save request", false);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&saveButton_));
}

// ------------------------------------------------------------------------------------------ SendFxPage
SendFxPage::SendFxPage(OpenBridgeProcessor& p) : Page(p)
{
    for (auto* s : {"DELAY", "REVERB", "CHORUS"}) {
        auto panel = std::make_unique<Panel>(s);
        addAndMakeVisible(*panel);
        panels_.push_back(std::move(panel));
    }
}

void SendFxPage::resized()
{
    // SEND FX page (page coordinates)
    const juce::Rectangle<int> boxes[] = {{3, 0, 731, 309}, {743, 0, 751, 309}, {3, 315, 652, 306}};
    for (size_t i = 0; i < panels_.size(); ++i) panels_[i]->setBounds(boxes[i]);
    if (!controls_.empty()) return;
    auto& delay = *panels_[0];
    auto& reverb = *panels_[1];
    auto& chorus = *panels_[2];
    // helpers in page coordinates
    auto local = [&](Panel& pnl, int x, int y) { return juce::Point<int>(x - pnl.getX(), y - pnl.getY()); };
    auto group = [&](Panel& pnl, const Tint& t, const juce::String& title, int x0, int y0, int x1, int y1) {
        auto g = std::make_unique<Group>(title, t);
        const auto o = local(pnl, x0, y0);
        g->setBounds(o.x, o.y, x1 - x0, y1 - y0);
        pnl.addAndMakeVisible(*g);
        g->toBack();
        groups_.push_back(std::move(g));
    };
    auto graph = [&](Panel& pnl, const Tint& t, Graph::Kind k, std::vector<std::string> keys, int x0, int y0, int x1, int y1) {
        auto g = std::make_unique<Graph>(proc_, 0, k, std::move(keys));
        g->tint = &t;
        g->fill = false;
        const auto o = local(pnl, x0, y0);
        g->setBounds(o.x, o.y, x1 - x0, y1 - y0);
        pnl.addAndMakeVisible(*g);
        graphs_.push_back(std::move(g));
    };
    auto val = [&](Panel& pnl, const char* key, const char* label, int cx, int labelY, int w = 84) {
        const auto o = local(pnl, cx, labelY - 11);
        make(pnl, paramIndex(key), 0, centred(o.x, o.y, w, 60), Value, label);
    };
    auto knob = [&](Panel& pnl, const char* key, const char* label, int cx, int labelY) {
        const auto o = local(pnl, cx, labelY - 12);
        make(pnl, paramIndex(key), 0, centred(o.x, o.y, 84, 100), KnobK, label);
    };
    auto send = [&](Panel& pnl, const char* key, const char* label, int cx, int labelY, int icon) {
        const auto o = local(pnl, cx, labelY - 12);
        if (auto* c = static_cast<SendIcon*>(make(pnl, paramIndex(key), 0, centred(o.x, o.y, 84, 100), Send, label))) c->icon = icon;
    };

    // DELAY (warm)
    tint_ = &Tints::filter;
    group(delay, Tints::filter, {}, 14, 46, 408, 300);
    graph(delay, Tints::filter, Graph::Kind::DelayEcho, {"FX_DelayTime", "FX_DelayFeedback", "FX_DelayStereoWidth"}, 30, 71, 396, 227);
    val(delay, "FX_DelayTime", "Time", 77, 245);
    {
        const auto o = local(delay, 155, 245);
        auto l = std::make_unique<TextLine>();
        l->setText("Pingpong");
        l->size = 15.0f;
        l->justification = juce::Justification::centred;
        l->setBounds(centred(o.x, o.y - 11, 90, 22));
        delay.addAndMakeVisible(*l);
        extras_.push_back(std::move(l));
        make(delay, paramIndex("FX_DelayMode"), 0, centred(o.x, o.y + 12, 46, 22), Switch);
    }
    val(delay, "FX_DelayStereoWidth", "Stereo Width", 254, 245, 100);
    val(delay, "FX_DelayFeedback", "Feedback", 343, 245);
    group(delay, Tints::filter, "Feedback Filter", 420, 46, 618, 300);
    graph(delay, Tints::filter, Graph::Kind::BandPass, {"FX_DelayHpFilter", "FX_DelayLpFilter"}, 445, 102, 595, 221);
    val(delay, "FX_DelayHpFilter", "HP Filter", 480, 245, 70);
    val(delay, "FX_DelayLpFilter", "LP Filter", 558, 245, 70);
    send(delay, "FX_DelayReverbSend", "Reverb", 677, 71, 1);
    knob(delay, "FX_DelayMixLevel", "Volume", 677, 197);

    // REVERB (blue)
    tint_ = &Tints::fx;
    group(reverb, Tints::fx, {}, 752, 46, 952, 300);
    graph(reverb, Tints::fx, Graph::Kind::ReverbSpace, {"FX_ReverbDecayTime", "FX_ReverbPreDelay"}, 777, 50, 927, 222);
    val(reverb, "FX_ReverbPreDelay", "Pre-Delay", 810, 245, 70);
    val(reverb, "FX_ReverbDecayTime", "Decay Time", 891, 245, 80);
    group(reverb, Tints::fx, "Feedback Shelving", 962, 46, 1162, 300);
    graph(reverb, Tints::fx, Graph::Kind::Shelf, {"FX_ReverbShelvingFrequency", "FX_ReverbShelvingGain"}, 984, 102, 1140, 190);
    val(reverb, "FX_ReverbShelvingFrequency", "Frequency", 1022, 245, 76);
    val(reverb, "FX_ReverbShelvingGain", "Gain", 1102, 245, 70);
    group(reverb, Tints::fx, "Input Filtering", 1172, 46, 1372, 300);
    graph(reverb, Tints::fx, Graph::Kind::BandPass, {"FX_ReverbHpFilter", "FX_ReverbLpFilter"}, 1195, 102, 1350, 221);
    val(reverb, "FX_ReverbHpFilter", "HP Filter", 1232, 245, 70);
    val(reverb, "FX_ReverbLpFilter", "LP Filter", 1310, 245, 70);
    knob(reverb, "FX_ReverbMixVolume", "Volume", 1438, 197);

    // CHORUS (lilac)
    tint_ = &Tints::mod;
    knob(chorus, "FX_ChorusDepth", "Depth", 66, 380);
    knob(chorus, "FX_ChorusWidth", "Width", 171, 380);
    group(chorus, Tints::mod, "Chorus Width", 223, 359, 433, 480);
    graph(chorus, Tints::mod, Graph::Kind::ChorusWidth, {"FX_ChorusWidth"}, 240, 390, 416, 470);
    knob(chorus, "FX_ChorusSpeed", "Speed", 66, 511);
    knob(chorus, "FX_ChorusHighPass", "HP Filter", 171, 511);
    group(chorus, Tints::mod, "High-Pass Filter", 223, 490, 433, 611);
    graph(chorus, Tints::mod, Graph::Kind::HighPass, {"FX_ChorusHighPass"}, 240, 528, 416, 600);
    send(chorus, "FX_ChorusDelaySend", "Delay", 485, 380, 0);
    send(chorus, "FX_ChorusReverbSend", "Reverb", 590, 380, 1);
    knob(chorus, "FX_ChorusMixVolume", "Volume", 590, 511);
}

// ------------------------------------------------------------------------------------------ MixerPage
namespace {
// Text and controls shown but not connected yet (compressor routing, "Audio to:").
class StaticText : public juce::Component, public juce::SettableTooltipClient {
public:
    StaticText(juce::String t, float size, juce::uint32 colour, bool boxed = false)
        : text_(std::move(t)), size_(size), colour_(colour), boxed_(boxed)
    {
    }
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        if (boxed_) {
            g.setColour(juce::Colour(0xff2f2f2f));
            g.fillRect(r);
            g.setColour(juce::Colour(Palette::outline).withAlpha(0.6f));
            g.drawRect(r, 1.0f);
        }
        g.setColour(juce::Colour(colour_));
        g.setFont(uiFont(size_, boxed_));
        g.drawText(text_, r, juce::Justification::centred);
    }

private:
    juce::String text_;
    float size_;
    juce::uint32 colour_;
    bool boxed_;
};
} // namespace

MixerPage::MixerPage(OpenBridgeProcessor& p) : Page(p)
{
    for (auto* panel : {&routing_, &compressor_, &extIn_, &mixer_}) addAndMakeVisible(*panel);
}

void MixerPage::resized()
{
    // MIXER page. Positions are given in pixels at 100 % zoom (panels' top at y 103) and converted to
    // this layout's coordinates.
    auto PX = [](float sx) { return (int)std::lround((sx - 9.0f) / 0.8f + 3.0f); };
    auto PY = [](float sy) { return (int)std::lround((sy - 103.0f) / 0.8f); };
    auto rect = [&](float x0, float y0, float x1, float y1) { return juce::Rectangle<int>(PX(x0), PY(y0), PX(x1) - PX(x0), PY(y1) - PY(y0)); };
    routing_.setBounds(rect(9, 103, 532, 348));
    compressor_.setBounds(rect(538, 103, 1197, 348));
    extIn_.setBounds(rect(9, 353, 225, 812));
    mixer_.setBounds(rect(231, 353, 1198, 812));
    if (!controls_.empty()) return;
    auto in = [&](Panel& pnl, juce::Rectangle<int> page) { return page.translated(-pnl.getX(), -pnl.getY()); };
    auto text = [&](Panel& pnl, const juce::String& t, juce::Rectangle<int> page, float size = 15.0f, juce::uint32 colour = Palette::text,
                    bool boxed = false) {
        auto c = std::make_unique<StaticText>(t, size, colour, boxed);
        c->setBounds(in(pnl, page));
        if (boxed) c->setTooltip("Not available yet in OpenBridge");
        pnl.addAndMakeVisible(*c);
        extras_.push_back(std::move(c));
    };
    auto knob = [&](Panel& pnl, const char* key, const char* label, float sx, float labelSy) {
        make(pnl, paramIndex(key), 0, in(pnl, centred(PX(sx), PY(labelSy) - 12, 84, 100)), KnobK, label);
    };
    auto graph = [&](Panel& pnl, Graph::Kind k, std::vector<std::string> keys, juce::Rectangle<int> page) {
        auto g = std::make_unique<Graph>(proc_, 0, k, std::move(keys));
        g->tint = &Tints::mod;
        g->fill = false;
        g->setBounds(in(pnl, page));
        pnl.addAndMakeVisible(*g);
        graphs_.push_back(std::move(g));
    };
    auto group = [&](Panel& pnl, juce::Rectangle<int> page) {
        auto g = std::make_unique<Group>("", Tints::mod);
        g->setBounds(in(pnl, page));
        pnl.addAndMakeVisible(*g);
        g->toBack();
        groups_.push_back(std::move(g));
    };
    auto meter = [&](Panel& pnl, const std::string& output, juce::Rectangle<int> page) {
        const int ch = proc_.outputChannelIndex(output);
        if (ch < 0) return;
        auto m = std::make_unique<LevelMeter>(proc_, ch);
        m->setBounds(in(pnl, page));
        pnl.addAndMakeVisible(*m);
        meters_.push_back(std::move(m));
    };
    tint_ = &Tints::mod;

    // ---- compressor routing (inputs of the compressor: not mapped yet)
    text(routing_, "Tracks", rect(150, 170, 250, 186));
    text(routing_, "", rect(342, 165, 344, 308), 1.0f, Palette::text, true); // separator
    text(routing_, "", rect(386, 201, 394, 203), 1.0f, Palette::text, true); // L-R link

    for (int t = 0; t < 16; ++t) {
        const float x = 75.0f + 32.0f * (float)(t % 8), y = t < 8 ? 190.0f : 260.0f;
        text(routing_, juce::String(t + 1), rect(x, y, x + 24, y + 24), 13.0f, Palette::textDim, true);
    }
    {
        struct Arrow : juce::Component {
            void paint(juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat();
                juce::Path p;
                p.startNewSubPath(r.getX(), r.getY());
                p.lineTo(r.getRight() - 8, r.getY());
                p.lineTo(r.getRight(), r.getCentreY());
                p.lineTo(r.getRight() - 8, r.getBottom());
                p.lineTo(r.getX(), r.getBottom());
                p.lineTo(r.getX() + 6, r.getCentreY());
                p.closeSubPath();
                g.setColour(juce::Colour(0xff9a8f9c));
                g.fillPath(p);
                g.setColour(juce::Colour(0xfff0f0f0));
                g.setFont(uiFont(14.0f));
                g.drawText("COMPRESSOR", r, juce::Justification::centred);
            }
        };
        auto a = std::make_unique<Arrow>();
        a->setBounds(in(routing_, rect(75, 230, 466, 245)));
        routing_.addAndMakeVisible(*a);
        extras_.push_back(std::move(a));
    }
    text(routing_, "External IN", rect(345, 170, 405, 186), 14.0f);
    text(routing_, "L", rect(362, 190, 386, 214), 13.0f, Palette::textDim, true);
    text(routing_, "R", rect(394, 190, 418, 214), 13.0f, Palette::textDim, true);
    text(routing_, "Send", rect(420, 170, 455, 186), 14.0f);
    text(routing_, "FX", rect(425, 190, 449, 214), 13.0f, Palette::textDim, true);

    // ---- compressor
    knob(compressor_, "FX_CompressorThreshold", "Threshold", 577, 158);
    knob(compressor_, "FX_CompressorRatio", "Ratio", 577, 253);
    group(compressor_, rect(610, 142, 828, 330));
    graph(compressor_, Graph::Kind::CompCurve, {"FX_CompressorThreshold", "FX_CompressorRatio"}, rect(630, 190, 808, 310));
    knob(compressor_, "FX_CompressorAttack", "Attack", 871, 158);
    knob(compressor_, "FX_CompressorRelease", "Release", 871, 253);
    text(compressor_, "", rect(832, 142, 839, 330), 1.0f, Palette::text, true); // separator bar
    group(compressor_, rect(905, 142, 1124, 330));
    graph(compressor_, Graph::Kind::BandPass, {"FX_CompressorSidechainFilter", "FX_CompressorSidechainFilter"}, rect(918, 160, 1110, 245));
    text(compressor_, "Sidechain Source", rect(915, 270, 1015, 286), 14.0f);
    make(compressor_, paramIndex("FX_CompressorSidechainSource"), 0, in(compressor_, centred(PX(965), PY(288), 90, 26)), Value, {});
    if (auto* v = dynamic_cast<ValueBox*>(controls_.back().get())) v->showLabel = false;
    make(compressor_, paramIndex("FX_CompressorSidechainFilter"), 0, in(compressor_, centred(PX(1076), PY(270), 84, 60)), Value, "Frequency");
    knob(compressor_, "FX_CompressorMakeupGain", "Gain", 1160, 158);
    knob(compressor_, "FX_CompressorDryWetMix", "Dry/Wet", 1160, 253);

    // ---- channel strip helper (number, Audio to:, buttons, pan, meter, fader)
    auto strip = [&](Panel& pnl, float x0, const juce::String& name, const juce::String& audioTo, const char* levelKey, int track,
                     const std::string& output, bool trackButtons) {
        text(pnl, name, rect(x0, 385, x0 + 42, 400), 14.0f);
        text(pnl, "Audio to:", rect(x0 - 4, 403, x0 + 46, 417), 12.5f);
        text(pnl, audioTo, rect(x0 + 2, 418, x0 + 40, 434), 13.0f, Palette::text, true);
        if (trackButtons) {
            if (auto* sB = static_cast<Toggle*>(make(pnl, paramIndex("Settings_TrackSolo"), track, in(pnl, rect(x0 + 2, 437, x0 + 40, 452)), Letter, "Solo")))
                sB->onColour = 0xfffafafa;
            // Two mutes: the track mute (global, whatever pattern plays) and the pattern
            // mute (saved in the pattern). Labelled M / P here so they can be told apart.
            if (auto* c = static_cast<Toggle*>(make(pnl, paramIndex("Settings_TrackMute"), track, in(pnl, rect(x0 + 2, 455, x0 + 20, 470)), Letter, "M")))
                c->setTooltip("Mute: track " + juce::String(track + 1) + " stays muted whatever pattern plays");
            if (auto* c = static_cast<Toggle*>(make(pnl, paramIndex("Kit_PatternTrackMute"), track, in(pnl, rect(x0 + 22, 455, x0 + 40, 470)), Letter, "P")))
                c->setTooltip("Pattern mute: saved in the current pattern, changes with the pattern");
            make(pnl, paramIndex("Sound_AmpPan"), track, in(pnl, rect(x0 + 2, 476, x0 + 40, 488)), HBar, "-");
            if (auto* b = dynamic_cast<Bar*>(controls_.back().get())) b->compact = true;
        }
        meter(pnl, output, rect(x0 + 3, 492, x0 + 11, 800));
        if (auto* f = static_cast<Fader*>(make(pnl, paramIndex(levelKey), track, in(pnl, rect(x0 + 14, 492, x0 + 40, 800)), FaderK, "-")))
            f->showValue = false;
    };

    // ---- EXT-IN
    text(extIn_, "Dual Mono", rect(128, 358, 190, 372), 14.0f);
    make(extIn_, paramIndex("FX_ExtInDualMono"), 0, in(extIn_, rect(192, 358, 218, 372)), Switch);
    text(extIn_, "Stereo Input", rect(68, 384, 165, 398), 14.0f);
    text(extIn_, "Audio to:", rect(66, 403, 116, 417), 12.5f);
    text(extIn_, "20", rect(70, 420, 113, 436), 13.0f, Palette::text, true);
    make(extIn_, paramIndex("FX_ExtInStereoChorusSend"), 0, in(extIn_, centred(PX(141), PY(403), 70, 46)), Value, "Chorus");
    make(extIn_, paramIndex("FX_ExtInStereoDelaySend"), 0, in(extIn_, centred(PX(141), PY(440), 70, 46)), Value, "Delay");
    make(extIn_, paramIndex("FX_ExtInStereoReverbSend"), 0, in(extIn_, centred(PX(141), PY(482), 70, 46)), Value, "Reverb");
    make(extIn_, paramIndex("FX_ExtInStereoInputPan"), 0, in(extIn_, rect(70, 476, 112, 488)), HBar, "-");
    if (auto* b = dynamic_cast<Bar*>(controls_.back().get())) b->compact = true;
    meter(extIn_, "External In", rect(73, 492, 81, 800));
    if (auto* f = static_cast<Fader*>(make(extIn_, paramIndex("FX_ExtInStereoInputLevel"), 0, in(extIn_, rect(84, 492, 110, 800)), FaderK, "-")))
        f->showValue = false;

    // ---- MIXER: 16 tracks, the sends and the pattern (master)
    for (int t = 0; t < 16; ++t)
        strip(mixer_, 239.0f + 47.0f * (float)t, juce::String(t + 1), juce::String(t + 1), "Kit_TrackLevel", t, "Track " + std::to_string(t + 1), true);
    strip(mixer_, 991, "Delay", "17", "FX_DelayMixLevel", 0, "Delay", false);
    strip(mixer_, 1038, "Reverb", "18", "FX_ReverbMixVolume", 0, "Reverb", false);
    strip(mixer_, 1085, "Chorus", "19", "FX_ChorusMixVolume", 0, "Chorus", false);
    strip(mixer_, 1132, "Pattern", "LR", "FX_MasterLevel", 0, "Main", false);
    text(mixer_, "OVDR", rect(1132, 437, 1174, 451), 13.0f);
    make(mixer_, paramIndex("FX_MasterOverdrive"), 0, in(mixer_, centred(PX(1153), PY(450), 50, 40)), Value, {});
    if (auto* v = dynamic_cast<ValueBox*>(controls_.back().get())) v->showLabel = false;
}

// ------------------------------------------------------------------------------------------ KitPage
KitPage::KitPage(OpenBridgeProcessor& p) : Page(p) {}

void KitPage::paint(juce::Graphics& g)
{
    // panel frame like the other pages, one row per track
    auto r = juce::Rectangle<int>(3, 0, 1491, 893);
    g.setColour(C(Palette::panel));
    g.fillRect(r);
    g.setColour(C(Palette::panelHeader));
    g.fillRect(r.withHeight(Panel::kHeader + 2).reduced(2));
    g.setColour(C(Palette::panelBorder));
    g.drawRect(r.toFloat().reduced(0.5f), 1.6f);
    g.setColour(C(Palette::panelTitle));
    g.setFont(uiFont(16.0f));
    g.drawText("KIT", r.withHeight(Panel::kHeader + 2).withTrimmedLeft(12), juce::Justification::centredLeft);
    auto area = r.withTrimmedTop(Panel::kHeader + 10).reduced(10, 8);
    g.setColour(C(Tints::synth.group));
    g.fillRoundedRectangle(area.toFloat(), 2.0f);
    area.reduce(14, 10);
    g.setFont(uiFont(15.0f));
    g.setColour(C(Tints::synth.title));
    auto head = area.removeFromTop(30);
    const int cols[] = {120, 330, 220, 220};
    const char* titles[] = {"Track", "Preset", "Machine", "Filter"};
    for (int i = 0; i < 4; ++i) g.drawText(titles[i], head.removeFromLeft(cols[i]), juce::Justification::centredLeft);
    auto& m = proc_.model();
    const int rowH = 48;
    for (int t = 0; t < 16; ++t) {
        auto row = area.removeFromTop(rowH);
        if (t % 2 == 0) {
            g.setColour(C(Palette::background).withAlpha(0.35f));
            g.fillRect(row);
        }
        g.setColour(C(Palette::text));
        g.drawText("TRK " + juce::String(t + 1), row.removeFromLeft(cols[0]).withTrimmedLeft(6), juce::Justification::centredLeft);
        g.setFont(uiFont(15.0f, true));
        g.drawText(juce::String(m.soundName(t)).toUpperCase(), row.removeFromLeft(cols[1]), juce::Justification::centredLeft);
        g.setFont(uiFont(15.0f));
        g.setColour(C(Tints::synth.accent));
        g.drawText(m.trackLoaded(t) ? ob::machineName(m.machine(t)) : "-", row.removeFromLeft(cols[2]), juce::Justification::centredLeft);
        g.setColour(C(Tints::filter.accent));
        g.drawText(m.trackLoaded(t) ? ob::filterMachineName(m.filterMachine(t)) : "-", row.removeFromLeft(cols[3]),
                   juce::Justification::centredLeft);
        g.setColour(C(Palette::textDim));
        g.drawText(ob::presetTagList(m.soundTags(t)), row, juce::Justification::centredLeft);
    }
}

} // namespace obui
