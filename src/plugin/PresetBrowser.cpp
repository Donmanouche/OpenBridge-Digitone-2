#include "PresetBrowser.h"

namespace obui {

namespace {
using Col = PresetBrowserPage::Colours;
juce::Colour C(juce::uint32 c) { return juce::Colour(c); }

constexpr int kRowH = 25;
constexpr int kRightX = 629, kRightW = 866;

// Column separators of the two lists (x relative to the list).
constexpr int kPoolCols[] = {57, 297, 531, 582};
constexpr int kListCols[] = {57, 386, 754, 818};

juce::String upper(const std::string& s) { return juce::String(s).toUpperCase(); }

void drawLock(juce::Graphics& g, juce::Rectangle<float> r)
{
    // padlock: shackle + body
    const float w = 13.0f, h = 17.0f;
    auto b = juce::Rectangle<float>(w, h).withCentre(r.getCentre());
    juce::Path shackle;
    shackle.addCentredArc(b.getCentreX(), b.getY() + 5.0f, 4.2f, 4.2f, 0.0f, -juce::MathConstants<float>::halfPi,
                          juce::MathConstants<float>::halfPi, true);
    shackle.lineTo(b.getCentreX() + 4.2f, b.getY() + 8.0f);
    shackle.startNewSubPath(b.getCentreX() - 4.2f, b.getY() + 5.0f);
    shackle.lineTo(b.getCentreX() - 4.2f, b.getY() + 8.0f);
    g.strokePath(shackle, juce::PathStrokeType(2.0f));
    g.fillRoundedRectangle(b.withTrimmedTop(7.5f), 1.5f);
}

void drawHeaderRow(juce::Graphics& g, juce::Rectangle<int> r, const std::vector<std::pair<int, const char*>>& labels,
                   const int* seps, int nSeps)
{
    g.setFont(uiFont(13.5f, true));
    g.setColour(C(Col::header));
    for (auto& [x, text] : labels) g.drawText(text, r.getX() + x, r.getY(), 200, r.getHeight(), juce::Justification::centredLeft);
    g.setColour(C(Col::separator));
    for (int i = 0; i < nSeps; ++i) g.fillRect(r.getX() + seps[i], r.getY() + 3, 2, r.getHeight() - 6);
}

juce::ScaledImage dragImage(const juce::String& text)
{
    juce::Image img(juce::Image::ARGB, 200, kRowH, true);
    juce::Graphics g(img);
    g.setColour(C(Col::selection).withAlpha(0.9f));
    g.fillRect(img.getBounds());
    g.setColour(C(Col::text));
    g.setFont(uiFont(13.5f, true));
    g.drawText(text, img.getBounds().reduced(8, 0), juce::Justification::centredLeft);
    return juce::ScaledImage(img);
}

bool parseDrive(const juce::var& d, int& bank, int& slot)
{
    const auto s = d.toString();
    if (!s.startsWith("drive:")) return false;
    auto parts = juce::StringArray::fromTokens(s.substring(6), ":", "");
    if (parts.size() != 2) return false;
    bank = parts[0].getIntValue();
    slot = parts[1].getIntValue();
    return bank >= 0 && bank < 8 && slot >= 1 && slot <= 256;
}
} // namespace

// ------------------------------------------------------------------------------------------ track tiles
class PresetBrowserPage::TrackTile : public juce::Component, public juce::DragAndDropTarget {
public:
    TrackTile(PresetBrowserPage& page, int track) : page_(page), track_(track) {}
    juce::String name;
    bool hover = false;

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour(C(Col::tile).brighter(hover ? 0.25f : 0.0f));
        g.fillRect(r);
        if (page_.activeTrack() == track_) {
            g.setColour(C(Col::activeRim));
            g.drawRect(r, 2.0f);
        }
        g.setColour(C(Col::text));
        g.setFont(uiFont(13.5f));
        g.drawText("TRK " + juce::String(track_ + 1), 5, 3, getWidth() - 8, 18, juce::Justification::centredLeft);
        g.setFont(uiFont(13.5f, true));
        g.drawText(name, 5, 21, getWidth() - 8, 18, juce::Justification::centredLeft, true);
    }
    void mouseDown(const juce::MouseEvent&) override { page_.setActiveTrack(track_); }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() < 5 || page_.isDragAndDropActive()) return;
        page_.startDragging("track:" + juce::String(track_), this, dragImage("TRK " + juce::String(track_ + 1) + "  " + name));
    }
    bool isInterestedInDragSource(const SourceDetails& d) override
    {
        int b, s;
        return parseDrive(d.description, b, s);
    }
    void itemDragEnter(const SourceDetails&) override { hover = true, repaint(); }
    void itemDragExit(const SourceDetails&) override { hover = false, repaint(); }
    void itemDropped(const SourceDetails& d) override
    {
        hover = false;
        repaint();
        int b, s;
        if (parseDrive(d.description, b, s)) page_.loadIntoTrack(track_, b, s);
    }

private:
    PresetBrowserPage& page_;
    const int track_;
};

// ------------------------------------------------------------------------------------------ preset pool
class PresetBrowserPage::PoolList : public juce::Component {
public:
    int selected = 0;
    void paint(juce::Graphics& g) override
    {
        const auto clip = g.getClipBounds();
        for (int i = std::max(0, clip.getY() / kRowH); i < 128 && i * kRowH < clip.getBottom(); ++i) {
            auto row = juce::Rectangle<int>(0, i * kRowH, getWidth(), kRowH);
            if (i == selected) {
                g.setColour(C(Col::selection));
                g.fillRect(row);
            }
            g.setColour(C(Col::text));
            g.setFont(uiFont(13.5f, true));
            g.drawText(juce::String(i + 1), 0, row.getY(), kPoolCols[0] - 4, kRowH, juce::Justification::centredRight);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        selected = juce::jlimit(0, 127, e.y / kRowH);
        repaint();
    }
};

// ------------------------------------------------------------------------------------------ +Drive list
class PresetBrowserPage::LibraryList : public juce::Component, public juce::DragAndDropTarget {
public:
    explicit LibraryList(PresetBrowserPage& page) : page_(page) {}

    struct Row {
        int bank, slot;
        bool empty;
        juce::String name, tags;
        bool tagsKnown, writeProtected;
    };
    std::vector<Row> rows;
    bool bankWritable = false;
    int selected = -1, dropRow = -1;
    juce::String placeholder;

    void paint(juce::Graphics& g) override
    {
        if (rows.empty() && placeholder.isNotEmpty()) {
            g.setColour(C(Col::dim));
            g.setFont(uiFont(13.5f));
            g.drawText(placeholder, getLocalBounds().removeFromTop(kRowH * 2).withTrimmedLeft(4), juce::Justification::centredLeft);
        }
        const auto clip = g.getClipBounds();
        for (int i = std::max(0, clip.getY() / kRowH); i < (int)rows.size() && i * kRowH < clip.getBottom(); ++i) {
            const auto& r = rows[(size_t)i];
            auto row = juce::Rectangle<int>(0, i * kRowH, getWidth(), kRowH);
            if (i == selected || i == dropRow) {
                g.setColour(i == dropRow ? C(Col::activeRim).withAlpha(0.5f) : C(Col::selection));
                g.fillRect(row);
            }
            g.setColour(C(Col::text));
            g.setFont(uiFont(13.5f, true));
            g.drawText(juce::String::charToString((juce::juce_wchar)('A' + r.bank)) + juce::String(r.slot).paddedLeft('0', 3),
                       row.getX() + 1, row.getY(), kListCols[0], kRowH, juce::Justification::centredLeft);
            if (r.empty) continue;
            g.drawText(r.name, row.getX() + kListCols[0] + 6, row.getY(), kListCols[1] - kListCols[0] - 8, kRowH,
                       juce::Justification::centredLeft, true);
            g.setFont(uiFont(13.5f));
            g.setColour(r.tagsKnown ? C(Col::text) : C(Col::dim));
            g.drawText(r.tagsKnown ? r.tags : juce::String("..."), row.getX() + kListCols[1] + 3, row.getY(),
                       kListCols[2] - kListCols[1] - 8, kRowH, juce::Justification::centredLeft, true);
            if (r.writeProtected) {
                g.setColour(C(Col::text));
                drawLock(g, juce::Rectangle<float>((float)kListCols[2], (float)row.getY(), (float)(kListCols[3] - kListCols[2]),
                                                   (float)kRowH));
            }
        }
    }

    int rowAt(int y) const
    {
        const int i = y / kRowH;
        return y >= 0 && i < (int)rows.size() ? i : -1;
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        selected = rowAt(e.y);
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        const int i = rowAt(e.getMouseDownY());
        if (i < 0 || rows[(size_t)i].empty || e.getDistanceFromDragStart() < 5 || page_.isDragAndDropActive()) return;
        const auto& r = rows[(size_t)i];
        page_.startDragging("drive:" + juce::String(r.bank) + ":" + juce::String(r.slot), this, dragImage(r.name));
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const int i = rowAt(e.y);
        if (i >= 0 && !rows[(size_t)i].empty) page_.loadIntoTrack(page_.activeTrack(), rows[(size_t)i].bank, rows[(size_t)i].slot);
    }

    // A track dropped on an empty slot of a writable bank is saved there.
    bool canSaveAt(int i) const { return bankWritable && i >= 0 && rows[(size_t)i].empty; }
    // Saving a track by dropping it here is not available yet (see saveTrackTo).
    bool isInterestedInDragSource(const SourceDetails&) override { return false; }
    void itemDragMove(const SourceDetails& d) override
    {
        const int i = rowAt(d.localPosition.y);
        const int n = canSaveAt(i) ? i : -1;
        if (n != dropRow) dropRow = n, repaint();
    }
    void itemDragExit(const SourceDetails&) override { dropRow = -1, repaint(); }
    void itemDropped(const SourceDetails& d) override
    {
        const int i = rowAt(d.localPosition.y);
        dropRow = -1;
        repaint();
        if (!canSaveAt(i)) return;
        page_.saveTrackTo(d.description.toString().substring(6).getIntValue(), rows[(size_t)i].bank, rows[(size_t)i].slot);
    }

private:
    PresetBrowserPage& page_;
};

// ------------------------------------------------------------------------------------------ label grid
class PresetBrowserPage::TagGrid : public juce::Component {
public:
    explicit TagGrid(OpenBridgeProcessor& p) : proc_(p) {}
    std::function<void()> onChange;
    juce::Rectangle<int> cell(int bit) const
    {
        const float w = (float)getWidth() / 8.0f;
        return {(int)std::lround((bit % 8) * w), (bit / 8) * kRowH, (int)w, kRowH};
    }
    void paint(juce::Graphics& g) override
    {
        for (int b = 0; b < 32; ++b) {
            const bool on = (proc_.uiTagFilter >> b) & 1u;
            g.setColour(on ? C(Col::text) : C(Col::dim));
            g.setFont(uiFont(14.0f)); // selected labels are white, not bold
            g.drawText(ob::presetTagName(b), cell(b), juce::Justification::centred);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        for (int b = 0; b < 32; ++b)
            if (cell(b).contains(e.getPosition())) {
                proc_.uiTagFilter ^= 1u << b;
                repaint();
                if (onChange) onChange();
            }
    }

private:
    OpenBridgeProcessor& proc_;
};

// ------------------------------------------------------------------------------------------ buttons
void PresetBrowserPage::FlatButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds();
    switch (style_) {
    case Style::Tab:
        g.setColour(getToggleState() ? C(Col::text) : C(Col::dim).brighter(over ? 0.4f : 0.0f));
        g.setFont(uiFont(15.0f));
        g.drawText(getButtonText(), r, juce::Justification::centred);
        break;
    case Style::Link: {
        g.setColour(C(Col::text).darker(over ? 0.15f : 0.0f));
        const auto f = uiFont(16.0f, true);
        g.setFont(f);
        g.drawText(getButtonText(), r, juce::Justification::centredLeft);
        // chevron: up when the filters are shown ("HIDE"), down otherwise
        const float x = (float)f.getStringWidth(getButtonText()) + 4.0f, y = (float)r.getCentreY();
        const bool up = getButtonText().startsWith("HIDE");
        juce::Path p;
        p.startNewSubPath(x, up ? y + 2.5f : y - 2.5f);
        p.lineTo(x + 4.5f, up ? y - 2.5f : y + 2.5f);
        p.lineTo(x + 9.0f, up ? y + 2.5f : y - 2.5f);
        g.strokePath(p, juce::PathStrokeType(1.6f));
        break;
    }
    case Style::Outline:
        g.setColour(C(Col::background).brighter(down ? 0.15f : over ? 0.07f : 0.0f));
        g.fillRect(r);
        g.setColour(C(Col::dim));
        g.drawRect(r, 2);
        g.setColour(C(Col::text));
        g.setFont(uiFont(14.5f));
        g.drawText(getButtonText(), r, juce::Justification::centred);
        break;
    }
}

// ------------------------------------------------------------------------------------------ page
PresetBrowserPage::PresetBrowserPage(OpenBridgeProcessor& p) : proc_(p)
{
    library_ = proc_.presetLibrary();
    for (int t = 0; t < 16; ++t) {
        tiles_[(size_t)t] = std::make_unique<TrackTile>(*this, t);
        addAndMakeVisible(*tiles_[(size_t)t]);
    }
    pool_ = std::make_unique<PoolList>();
    pool_->setSize(605, 128 * kRowH);
    poolView_.setViewedComponent(pool_.get(), false);
    poolView_.setScrollBarsShown(true, false);
    poolView_.setScrollBarThickness(8);
    addAndMakeVisible(poolView_);

    list_ = std::make_unique<LibraryList>(*this);
    listView_.setViewedComponent(list_.get(), false);
    listView_.setScrollBarsShown(true, false);
    listView_.setScrollBarThickness(8);
    addAndMakeVisible(listView_);
    for (auto* v : {&poolView_, &listView_}) {
        v->getVerticalScrollBar().setColour(juce::ScrollBar::thumbColourId, C(Col::scrollbar));
        v->getVerticalScrollBar().setColour(juce::ScrollBar::trackColourId, C(Col::background));
    }

    tags_ = std::make_unique<TagGrid>(proc_);
    tags_->onChange = [this] { rebuildLibrary(); };
    addAndMakeVisible(*tags_);

    for (int b = 0; b < 8; ++b) {
        auto& t = bankTabs_[(size_t)b];
        t = std::make_unique<FlatButton>(juce::String::charToString((juce::juce_wchar)('A' + b)), FlatButton::Style::Tab);
        t->onClick = [this, b] {
            proc_.uiLibraryBank = b;
            library_->setPriorityBank(b);
            listView_.setViewPosition(0, 0);
            rebuildLibrary();
            repaint();
        };
        addAndMakeVisible(*t);
    }
    hideFilters_.onClick = [this] {
        proc_.uiHideFilters = !proc_.uiHideFilters;
        resized();
        repaint();
    };
    clearFilters_.onClick = [this] {
        proc_.uiTagFilter = 0;
        tags_->repaint();
        rebuildLibrary();
    };
    for (auto* b : {&hideFilters_, &clearFilters_}) addAndMakeVisible(*b);

    library_->setPriorityBank(proc_.uiLibraryBank);
    rebuildLibrary();
    timerCallback();
    startTimerHz(10);
}

PresetBrowserPage::~PresetBrowserPage() { stopTimer(); }

int PresetBrowserPage::activeTrack() const { return juce::jlimit(0, 15, proc_.uiActiveTrack); }

void PresetBrowserPage::setActiveTrack(int track)
{
    proc_.uiActiveTrack = track;
    for (auto& t : tiles_) t->repaint();
}

void PresetBrowserPage::loadIntoTrack(int track, int bank, int slot)
{
    setActiveTrack(track);
    // The device ignores the storage operations (class 0x43): read the preset over SysEx and
    // write it into the track.
    std::weak_ptr<ob::DeviceModel> model = proc_.modelPtr();
    library_->readPreset(bank, slot, [model, track](bool ok, std::vector<uint8_t> container) {
        if (auto m = model.lock(); m && ok) m->applySound(track, std::move(container));
    });
}

void PresetBrowserPage::saveTrackTo(int, int, int)
{
    // The device ignores the storage operation class 0x44; saving
    // will go through a SysEx file write once that is verified.
}

void PresetBrowserPage::rebuildLibrary()
{
    const int bank = juce::jlimit(0, 7, proc_.uiLibraryBank);
    const uint32_t filter = proc_.uiTagFilter;
    const auto presets = library_->bank(bank);
    std::vector<LibraryList::Row> rows;
    bool anyWritable = false, anyProtected = false;
    for (auto& p : presets) (p.writeProtected ? anyProtected : anyWritable) = true;
    if (filter) {
        for (auto& p : presets)
            if ((p.tags & filter) == filter)
                rows.push_back({p.bank, p.slot, false, upper(p.name), ob::presetTagList(p.tags), p.tagsKnown, p.writeProtected});
    } else {
        rows.resize(256);
        for (int s = 0; s < 256; ++s) rows[(size_t)s] = {bank, s + 1, true, {}, {}, true, false};
        for (auto& p : presets)
            rows[(size_t)p.slot - 1] = {p.bank, p.slot, false, upper(p.name), ob::presetTagList(p.tags), p.tagsKnown,
                                        p.writeProtected};
    }
    list_->bankWritable = anyWritable || !anyProtected;
    if (!library_->bankListed(bank)) {
        rows.clear();
        list_->placeholder = "Reading the +Drive...";
    } else list_->placeholder = filter ? "No preset with these labels in this bank" : "";
    list_->rows = std::move(rows);
    list_->selected = -1;
    list_->setSize(listView_.getMaximumVisibleWidth(), std::max(1, (int)list_->rows.size()) * kRowH);
    list_->repaint();
    for (int b = 0; b < 8; ++b) bankTabs_[(size_t)b]->setToggleState(b == bank, juce::dontSendNotification);
}

void PresetBrowserPage::timerCallback()
{
    for (int t = 0; t < 16; ++t) {
        auto name = upper(proc_.model().soundName(t));
        if (name != tiles_[(size_t)t]->name) {
            tiles_[(size_t)t]->name = name;
            tiles_[(size_t)t]->repaint();
        }
    }
    const auto gen = library_->generation();
    if (gen != libraryGen_) {
        libraryGen_ = gen;
        // keep the scroll position and the selection while the tags come in
        const auto pos = listView_.getViewPosition();
        const int sel = list_->selected;
        rebuildLibrary();
        list_->selected = sel;
        listView_.setViewPosition(pos);
        const juce::String st(library_->status());
        if (st != lastStatus_) {
            lastStatus_ = st;
            repaint();
        }
    }
}

void PresetBrowserPage::resized()
{
    // left: kit track presets and the project preset pool
    for (int t = 0; t < 16; ++t) tiles_[(size_t)t]->setBounds(12 + (t % 4) * 147, 49 + (t / 4) * 46, 142, 42);
    poolHeader_ = {0, 292, 597, 24};
    poolView_.setBounds(0, 322, 605, getHeight() - 322 - 10);
    pool_->setSize(poolView_.getMaximumVisibleWidth(), 128 * kRowH);
    layoutRight();
}

void PresetBrowserPage::layoutRight()
{
    hideFilters_.setButtonText(proc_.uiHideFilters ? "SHOW FILTERS" : "HIDE FILTERS");
    hideFilters_.setBounds(kRightX + 2, 43, 170, 24);
    clearFilters_.setBounds(kRightX + kRightW - 120, 41, 120, 29);
    const float w = kRightW / 8.0f;
    for (int b = 0; b < 8; ++b)
        bankTabs_[(size_t)b]->setBounds(kRightX + (int)std::lround(b * w), 78, (int)w, 26);
    const bool filters = !proc_.uiHideFilters;
    tags_->setVisible(filters);
    clearFilters_.setVisible(filters);
    tags_->setBounds(kRightX, 112, kRightW, 4 * kRowH);
    const int headerY = filters ? 239 : 117;
    listHeader_ = {kRightX, headerY, kRightW, 24};
    listView_.setBounds(kRightX, headerY + 31, kRightW + 5, getHeight() - headerY - 31 - 10);
    list_->setSize(listView_.getMaximumVisibleWidth(), list_->getHeight());
}

void PresetBrowserPage::paint(juce::Graphics& g)
{
    g.fillAll(C(Col::background));
    g.setFont(uiFont(17.0f, true));
    g.setColour(C(Col::kitTitle));
    g.drawText("KIT TRACK PRESETS", 12, 12, 400, 22, juce::Justification::centredLeft);
    g.drawText("PROJECT PRESET POOL", 12, 261, 400, 22, juce::Justification::centredLeft);
    g.setColour(C(Col::driveTitle));
    g.drawText("+DRIVE PRESET LIBRARY", kRightX + 3, 12, 400, 22, juce::Justification::centredLeft);
    if (lastStatus_.isNotEmpty()) {
        g.setColour(C(Col::dim));
        g.setFont(uiFont(13.0f));
        g.drawText(lastStatus_, kRightX + 230, 12, kRightW - 230, 22, juce::Justification::centredRight, true);
    }

    drawHeaderRow(g, poolHeader_, {{7, "SLOT"}, {65, "NAME"}, {303, "TAGS"}}, kPoolCols, 4);
    g.setColour(C(Col::dim));
    g.setFont(uiFont(13.0f));
    g.drawText("Pool contents are not readable from this OS version", poolHeader_.translated(0, -26).withTrimmedLeft(260),
               juce::Justification::centredRight, true);

    // filter area lines
    g.setColour(C(Col::line));
    g.fillRect(kRightX, 75, kRightW, 1);
    g.fillRect(kRightX, 106, kRightW, 1);
    drawHeaderRow(g, listHeader_, {{1, "BANK"}, {kListCols[0] + 6, "NAME"}, {kListCols[1] + 3, "TAGS"}, {kListCols[2] + 7, "WP"}},
                  kListCols, 4);
    g.setColour(C(Col::line));
    g.fillRect(listHeader_.getX(), listHeader_.getBottom() + 2, kRightW, 1);
}

} // namespace obui
