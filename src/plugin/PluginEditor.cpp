#include "PluginEditor.h"

#include "Diagrams.h"

#include <cmath>

namespace obui {

using C = juce::Colour;

LookAndFeel::LookAndFeel()
{
    setColour(juce::ComboBox::backgroundColourId, C(0xff2a2a2a));
    setColour(juce::ComboBox::textColourId, C(Palette::text));
    setColour(juce::ComboBox::outlineColourId, C(Palette::textDim));
    setColour(juce::ComboBox::arrowColourId, C(Palette::text));
    setColour(juce::PopupMenu::backgroundColourId, C(Palette::header));
    setColour(juce::PopupMenu::textColourId, C(Palette::text));
    setColour(juce::PopupMenu::highlightedBackgroundColourId, C(Palette::tabActive));
    setColour(juce::TextButton::textColourOffId, C(Palette::textDim));
    setColour(juce::TextButton::textColourOnId, C(Palette::text));
    setColour(juce::TooltipWindow::backgroundColourId, C(Palette::header));
    setColour(juce::TooltipWindow::textColourId, C(Palette::text));
}

void LookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    // Overbridge: dark box, light grey 2 px frame, bold white text, thin chevron
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRect(0, 0, w, h);
    g.setColour(C(Palette::outline));
    g.drawRect(0, 0, w, h, 2);
    const float ax = (float)w - 18.0f, ay = (float)h * 0.5f;
    juce::Path p;
    p.startNewSubPath(ax - 5, ay - 2.5f);
    p.lineTo(ax, ay + 2.5f);
    p.lineTo(ax + 5, ay - 2.5f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.strokePath(p, juce::PathStrokeType(1.6f));
}

void LookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(8, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

juce::Font LookAndFeel::getComboBoxFont(juce::ComboBox&) { return uiFont(15.5f, true); }
juce::Font LookAndFeel::getTextButtonFont(juce::TextButton& b, int)
{
    return b.getProperties()["outlined"] ? uiFont(15.5f) : uiFont(15.5f, true);
}

void LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    if (b.getProperties()["outlined"]) {
        // action buttons (PRESET panel: Load... / Save...): dark box with a light frame
        g.setColour(C(Palette::background).brighter(down ? 0.12f : over ? 0.06f : 0.0f));
        g.fillRect(b.getLocalBounds());
        g.setColour(C(Palette::outline).withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.6f));
        g.drawRect(b.getLocalBounds(), 2);
        return;
    }
    g.setColour(C(b.getToggleState() ? Palette::tabActive : Palette::header).brighter(over ? 0.05f : 0.0f));
    g.fillRect(b.getLocalBounds());
}

// ------------------------------------------------------------------------------------------ header
HeaderBar::HeaderBar(OpenBridgeProcessor& p) : proc_(p)
{
    for (int z : {50, 75, 100, 120, 150, 200}) zoom_.addItem(juce::String(z) + "%", z);
    zoom_.setSelectedId((int)std::lround(p.uiZoom * 100), juce::dontSendNotification);
    zoom_.onChange = [this] {
        if (onZoom) onZoom((float)zoom_.getSelectedId() / 100.0f);
    };
    addAndMakeVisible(zoom_);
    device_.onChange = [this] {
        const int i = device_.getSelectedItemIndex();
        if (i >= 0 && i < (int)deviceIds_.size()) proc_.selectDevice(deviceIds_[(size_t)i]);
    };
    addAndMakeVisible(device_);
    keyboard_.setTooltip("Show / hide keyboard");
    keyboard_.onClick = [this] {
        if (onKeyboard) onKeyboard();
    };
    addAndMakeVisible(keyboard_);
    for (auto* b : {&kitEditor_, &presetBrowser_}) {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(1);
        addAndMakeVisible(*b);
    }
    (proc_.uiPresetBrowser ? presetBrowser_ : kitEditor_).setToggleState(true, juce::dontSendNotification);
    kitEditor_.onClick = [this] { if (onPage) onPage(0); };
    presetBrowser_.onClick = [this] { if (onPage) onPage(1); };
    refreshDevices();
}

void HeaderBar::KeyboardButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    // piano keys: white keys with gaps, black keys on top
    const auto c = juce::Colour(0xff9a9a9a).brighter(down ? 0.5f : over ? 0.3f : 0.0f);
    auto r = getLocalBounds().toFloat().withSizeKeepingCentre(36.0f, 22.0f);
    const float kw = r.getWidth() / 5.0f;
    g.setColour(c);
    for (int k = 0; k < 5; ++k) g.fillRect(r.getX() + k * kw, r.getY() + 9.0f, kw - 2.0f, r.getHeight() - 9.0f);
    for (int k = 0; k < 6; ++k) {
        if (k == 2) continue;
        const float x = r.getX() + k * kw - 3.0f;
        if (x < r.getX() || x + 4.0f > r.getRight()) continue;
        g.fillRect(x, r.getY(), 4.0f, 13.0f);
    }
    g.fillRect(r.getX(), r.getY(), r.getWidth() - 2.0f, 2.0f);
}

void HeaderBar::setPresetBrowser(bool on)
{
    (on ? presetBrowser_ : kitEditor_).setToggleState(true, juce::dontSendNotification);
}

void HeaderBar::refreshDevices()
{
    std::vector<std::string> ids{""};
    juce::StringArray names{juce::String(proc_.desc().name)};
    if (auto* s = proc_.session()) {
        for (auto& d : s->availableDevices()) {
            ids.push_back(d.id());
            names.add(juce::String(d.desc->name) + "  [" + juce::String(d.serial.empty() ? d.id() : d.serial) + "]");
        }
    }
    if (ids == deviceIds_) return;
    deviceIds_ = ids;
    device_.clear(juce::dontSendNotification);
    device_.addItemList(names, 1);
    const auto want = proc_.selectedDeviceId().toStdString();
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == want) device_.setSelectedItemIndex((int)i, juce::dontSendNotification);
}

void HeaderBar::paint(juce::Graphics& g)
{
    g.fillAll(C(Palette::header));
    // logo, then "OpenBridge / Digitone II"
    obui::drawLogo(g, juce::Rectangle<float>(4, 3, 31, 31), C(Palette::accent));
    g.setColour(C(Palette::accent));
    const auto f = uiFont(19.5f, true);
    g.setFont(f);
    const juce::String brand = "OpenBridge", rest = " / " + juce::String(proc_.desc().name);
    const int bw = f.getStringWidth(brand);
    g.drawText(brand, 44, 0, bw + 2, getHeight(), juce::Justification::centredLeft);
    g.setFont(uiFont(19.5f));
    g.drawText(rest, 44 + bw, 0, 220, getHeight(), juce::Justification::centredLeft);
    // routing icon (fork)
    const float fx = (float)getWidth() - 337.0f, fy = 9.0f;
    g.setColour(C(0xff9a9a9a));
    g.fillEllipse(fx - 3, fy - 3, 6, 6);
    g.fillEllipse(fx + 9, fy - 1, 6, 6);
    g.fillEllipse(fx - 3, fy + 16, 6, 6);
    g.drawLine(fx, fy, fx, fy + 18, 2.0f);
    juce::Path p;
    p.startNewSubPath(fx + 12, fy + 2);
    p.quadraticTo(fx + 12, fy + 10, fx, fy + 12);
    g.strokePath(p, juce::PathStrokeType(2.0f));
}

void HeaderBar::resized()
{
    zoom_.setBounds(262, 6, 74, 23);
    device_.setBounds(346, 6, 290, 23);
    presetBrowser_.setBounds(getWidth() - 128, 0, 128, getHeight());
    kitEditor_.setBounds(getWidth() - 222, 0, 94, getHeight());
    keyboard_.setBounds(getWidth() - 310, 2, 50, getHeight() - 4);
}

// ------------------------------------------------------------------------------------------ tabs
juce::StringArray TrackTabs::names()
{
    juce::StringArray s{"KIT"};
    for (int i = 1; i <= 16; ++i) s.add("TRK " + juce::String(i));
    s.add("SEND FX");
    s.add("MIXER");
    return s;
}

juce::Rectangle<int> TrackTabs::tabBounds(int i) const
{
    // tab edges (pixels at 120 %)
    static const int edges[][2] = {{15, 75},     {83, 165},    {173, 255},   {263, 345},   {353, 435},
                                   {443, 525},   {533, 615},   {623, 705},   {713, 795},   {803, 885},
                                   {893, 988},   {997, 1092},  {1100, 1195}, {1203, 1298}, {1305, 1400},
                                   {1408, 1503}, {1510, 1605}, {1612, 1707}, {1715, 1785}};
    const float k = 1795.0f / 1500.0f;
    const int x0 = (int)std::lround(edges[i][0] / k), x1 = (int)std::lround(edges[i][1] / k);
    return {x0, 0, x1 - x0, getHeight()};
}

void TrackTabs::paint(juce::Graphics& g)
{
    g.fillAll(C(Palette::background));
    const auto n = names();
    g.setFont(uiFont(14.5f));
    for (int i = 0; i < n.size(); ++i) {
        auto r = tabBounds(i).toFloat();
        const bool on = i == selected_;
        g.setColour(C(on ? Palette::tabBorderActive : Palette::tabBorder));
        g.drawRect(r.reduced(0.75f), on ? 2.0f : 1.5f);
        g.setColour(on ? C(Palette::text) : C(Palette::textDim));
        g.drawText(n[i], r, juce::Justification::centred);
    }
}

void TrackTabs::mouseDown(const juce::MouseEvent& e)
{
    for (int i = 0; i < names().size(); ++i)
        if (tabBounds(i).contains(e.getPosition())) {
            setSelected(i);
            if (onChange) onChange(i);
        }
}

// ------------------------------------------------------------------------------------------ footer
void Footer::paint(juce::Graphics& g)
{
    g.fillAll(C(Palette::header));
    auto r = getLocalBounds().reduced(12, 0);
    g.setFont(uiFont(13.0f));
    juce::String status;
    if (auto* s = proc_.session()) status = s->statusText();
    if (!proc_.isAudioOwner()) status += "  (audio used by another OpenBridge instance)";
    if (proc_.model().connected()) status += "  -  control channel OK";
    if (!proc_.midiOutputOpen()) status += "  -  device MIDI port not found";
    g.setColour(C(Palette::textDim));
    g.drawText(status, r, juce::Justification::centredLeft);
    g.setColour(C(0xfff0f0f0));
    g.setFont(uiFont(25.0f, true).italicised());
    g.drawText("OpenBridge", r.withTrimmedRight(4), juce::Justification::centredRight);
}

} // namespace obui

// ------------------------------------------------------------------------------------------ editor
OpenBridgeEditor::OpenBridgeEditor(OpenBridgeProcessor& p)
    : AudioProcessorEditor(&p), proc_(p), header_(p), footer_(p)
{
    setLookAndFeel(&lnf_);
    addAndMakeVisible(content_);
    content_.addAndMakeVisible(header_);
    content_.addAndMakeVisible(tabs_);
    content_.addAndMakeVisible(footer_);
    content_.addChildComponent(keyboard_);
    keyboard_.setAvailableRange(24, 108); // C1 .. C8, keys sized to fill the editor's width
    keyboard_.setLowestVisibleKey(24);
    keyboard_.setColour(juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour(0xffd8d8d8));
    keyboard_.setColour(juce::MidiKeyboardComponent::blackNoteColourId, juce::Colour(0xff1e1e1e));
    keyboard_.setColour(juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour(0xff6d6d6d));
    keyboard_.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, juce::Colour(obui::Palette::accent).withAlpha(0.3f));
    keyboard_.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, juce::Colour(obui::Palette::accent).withAlpha(0.7f));
    keyboard_.setColour(juce::MidiKeyboardComponent::shadowColourId, juce::Colours::transparentBlack);
    keyboard_.setColour(juce::MidiKeyboardComponent::upDownButtonBackgroundColourId, juce::Colour(obui::Palette::header));
    keyboard_.setScrollButtonsVisible(false);
    keyState_.addListener(this);
    header_.onZoom = [this](float z) { applyZoom(z); };
    header_.onPage = [this](int page) {
        presetBrowser_ = proc_.uiPresetBrowser = page == 1;
        showPage(proc_.uiPage);
    };
    header_.onKeyboard = [this] {
        showKeyboard_ = proc_.uiKeyboard = !showKeyboard_;
        keyboard_.setVisible(showKeyboard_);
        resized();
    };
    presetBrowser_ = proc_.uiPresetBrowser;
    showKeyboard_ = proc_.uiKeyboard;
    keyboard_.setVisible(showKeyboard_);
    tabs_.onChange = [this](int i) { showPage(i); };
    tabs_.setSelected(proc_.uiPage);
    setResizable(false, false);
    showPage(proc_.uiPage);
    applyZoom(p.uiZoom);
}

OpenBridgeEditor::~OpenBridgeEditor()
{
    keyState_.removeListener(this);
    setLookAndFeel(nullptr);
}

void OpenBridgeEditor::showPage(int page)
{
    proc_.uiPage = page;
    if (page_) content_.removeChildComponent(page_.get());
    if (presetBrowser_) page_ = std::make_unique<obui::PresetBrowserPage>(proc_);
    else if (page == 0) page_ = std::make_unique<obui::KitPage>(proc_);
    else if (page <= 16) page_ = std::make_unique<obui::TrackPage>(proc_, page - 1);
    else if (page == 17) page_ = std::make_unique<obui::SendFxPage>(proc_);
    else page_ = std::make_unique<obui::MixerPage>(proc_);
    tabs_.setVisible(!presetBrowser_);
    content_.addAndMakeVisible(*page_);
    resized();
}

void OpenBridgeEditor::openPresetBrowser()
{
    // called from a page: replace it asynchronously
    juce::Component::SafePointer<OpenBridgeEditor> self(this);
    juce::MessageManager::callAsync([self] {
        if (!self) return;
        self->header_.setPresetBrowser(true);
        self->presetBrowser_ = self->proc_.uiPresetBrowser = true;
        self->showPage(self->proc_.uiPage);
    });
}

void OpenBridgeEditor::applyZoom(float z)
{
    proc_.uiZoom = z;
    content_.setTransform(juce::AffineTransform::scale(z * kScale));
    setSize((int)std::lround(kBaseWidth * z * kScale), (int)std::lround(kBaseHeight * z * kScale));
}

void OpenBridgeEditor::paint(juce::Graphics& g) { g.fillAll(juce::Colour(obui::Palette::background)); }

void OpenBridgeEditor::resized()
{
    content_.setBounds(0, 0, kBaseWidth, kBaseHeight);
    auto r = content_.getLocalBounds();
    header_.setBounds(r.removeFromTop(36));
    footer_.setBounds(r.removeFromBottom(40));
    if (showKeyboard_) {
        keyboard_.setBounds(r.removeFromBottom(80));
        // white keys between C1 and C8: 7 per octave + the last C
        keyboard_.setKeyWidth((float)keyboard_.getWidth() / (7.0f * 7.0f + 1.0f));
    }
    r.removeFromTop(21);
    tabs_.setBounds(r.removeFromTop(27));
    r.removeFromTop(6);
    if (page_ && presetBrowser_) {
        page_->setTransform({});
        page_->setBounds(header_.getBounds().getX(), header_.getBottom(), kBaseWidth, r.getBottom() - header_.getBottom());
    } else if (page_) {
        // pages are designed for 1500 x 900; with the keyboard shown they are scaled down vertically
        page_->setTransform({});
        page_->setBounds(r.getX(), r.getY(), kBaseWidth, 900);
        if (r.getHeight() < 900)
            page_->setTransform(juce::AffineTransform::scale(1.0f, (float)r.getHeight() / 900.0f, (float)r.getX(), (float)r.getY()));
    }
}

void OpenBridgeEditor::handleNoteOn(juce::MidiKeyboardState*, int, int note, float vel)
{
    const int ch = proc_.uiPage >= 1 && proc_.uiPage <= 16 ? proc_.uiPage : 1; // track n listens on channel n
    proc_.sendMidiToDevice(juce::MidiMessage::noteOn(ch, note, vel));
}

void OpenBridgeEditor::handleNoteOff(juce::MidiKeyboardState*, int, int note, float vel)
{
    const int ch = proc_.uiPage >= 1 && proc_.uiPage <= 16 ? proc_.uiPage : 1;
    proc_.sendMidiToDevice(juce::MidiMessage::noteOff(ch, note, vel));
}
