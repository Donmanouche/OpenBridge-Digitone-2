// Vector drawings of the editor: FM operator algorithms (FM Tone, FM Drum), oscillator modulation
// icons and the OpenBridge logo. Drawn in code, scaled to any rectangle.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace obui {

struct DiagramColours {
    juce::Colour line; // boxes, connections
    juce::Colour text; // operator names, X / Y
};

// FM Tone: algorithm 0..7 (operators A, B1, B2, C; outputs X and Y).
void drawFmToneAlgorithm(juce::Graphics& g, juce::Rectangle<float> area, int algorithm, DiagramColours c);
// FM Drum: algorithm 0..6 (operators A, B, C).
void drawFmDrumAlgorithm(juce::Graphics& g, juce::Rectangle<float> area, int algorithm, DiagramColours c);
// WaveTone oscillator modulation: 0 off, 1 ring mod, 2 ring mod fixed, 3 hard sync.
void drawOscModulation(juce::Graphics& g, juce::Rectangle<float> area, int mode, DiagramColours c);
// OpenBridge logo: the ring is the "O"; inside, a "B" whose two bowls are bridge arches laid on their side.
void drawLogo(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour);

} // namespace obui
