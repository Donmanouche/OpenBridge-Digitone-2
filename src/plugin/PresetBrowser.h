// Preset Browser page:
// KIT TRACK PRESETS (16 tracks, active track with an orange rim), PROJECT PRESET POOL (128 slots),
// +DRIVE PRESET LIBRARY (banks A–H, label grid, BANK / NAME / TAGS / WP list).
//
// Load a preset: drag it from the library onto a track, or double-click it (loads into the active
// track). The preset file is read over SysEx and written into the track (DeviceModel::applySound).
// Saving to the +Drive is not available yet: the device ignores the storage requests.
#pragma once

#include "Widgets.h"

#include <array>
#include <memory>
#include <vector>

namespace obui {

class PresetBrowserPage : public juce::Component, public juce::DragAndDropContainer, private juce::Timer {
public:
    explicit PresetBrowserPage(OpenBridgeProcessor& p);
    ~PresetBrowserPage() override;
    void paint(juce::Graphics&) override;
    void resized() override;

    // Colours of the Preset Browser.
    struct Colours {
        static constexpr juce::uint32 background = 0xff1e1e1e;
        static constexpr juce::uint32 tile = 0xff285765;
        static constexpr juce::uint32 activeRim = 0xffeb9f6a;
        static constexpr juce::uint32 kitTitle = 0xff285765;
        static constexpr juce::uint32 driveTitle = 0xff24604e;
        static constexpr juce::uint32 line = 0xff24604e;
        static constexpr juce::uint32 text = 0xfff0f0f0;
        static constexpr juce::uint32 dim = 0xff666666;
        static constexpr juce::uint32 header = 0xff656565;
        static constexpr juce::uint32 separator = 0xff464646;
        static constexpr juce::uint32 selection = 0xff646464;
        static constexpr juce::uint32 scrollbar = 0xff646464;
    };

    void loadIntoTrack(int track, int bank, int slot);
    void saveTrackTo(int track, int bank, int slot);
    void setActiveTrack(int track);
    int activeTrack() const;

    // Flat text buttons of the browser: bank tabs, "HIDE FILTERS", outlined "Clear filters".
    class FlatButton : public juce::Button {
    public:
        enum class Style { Tab, Link, Outline };
        FlatButton(const juce::String& text, Style s) : juce::Button(text), style_(s) {}
        void paintButton(juce::Graphics&, bool over, bool down) override;

    private:
        Style style_;
    };

private:
    class TrackTile;
    class PoolList;
    class LibraryList;
    class TagGrid;

    void timerCallback() override;
    void layoutRight();
    void rebuildLibrary();

    OpenBridgeProcessor& proc_;
    std::shared_ptr<ob::PresetLibrary> library_;
    uint64_t libraryGen_ = 0;
    juce::String lastStatus_;

    std::array<std::unique_ptr<TrackTile>, 16> tiles_;
    std::unique_ptr<PoolList> pool_;
    juce::Viewport poolView_;
    std::unique_ptr<LibraryList> list_;
    juce::Viewport listView_;
    std::unique_ptr<TagGrid> tags_;
    FlatButton hideFilters_{"", FlatButton::Style::Link}, clearFilters_{"Clear filters", FlatButton::Style::Outline};
    std::array<std::unique_ptr<FlatButton>, 8> bankTabs_;
    juce::Rectangle<int> poolHeader_, listHeader_;
};

} // namespace obui
