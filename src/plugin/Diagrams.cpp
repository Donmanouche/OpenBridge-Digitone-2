#include "Diagrams.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace obui {

namespace {

struct Op {
    const char* name;
    float col, row;
    bool feedback;
};

struct Algorithm {
    std::vector<Op> ops;
    std::vector<std::pair<int, int>> mods; // modulator -> modulated (indices in ops)
    std::vector<int> x, y;                 // carriers mixed to output X / Y (FM Drum: x = the outputs)
};

// FM Tone algorithms 1..8: operator positions (column, row), modulations, outputs, feedback.
const std::vector<Algorithm>& fmToneAlgorithms()
{
    static const std::vector<Algorithm> a = {
        // 1
        {{{"A", 0, 1, true}, {"B1", 1, 1, false}, {"B2", 1, 0, false}, {"C", 0, 2, false}}, {{0, 3}, {2, 1}, {1, 3}}, {3}, {1}},
        // 2
        {{{"A", 0, 0, false}, {"B1", 1, 1, false}, {"B2", 1, 0, true}, {"C", 0, 1, false}}, {{0, 3}, {2, 1}}, {3}, {1}},
        // 3
        {{{"A", 1, 0, true}, {"B1", 2, 1, false}, {"B2", 1, 1, false}, {"C", 0, 1, false}}, {{0, 3}, {0, 2}, {0, 1}}, {3, 2}, {1}},
        // 4
        {{{"A", 0, 2, false}, {"B1", 0, 1, false}, {"B2", 0, 0, true}, {"C", 0, 3, false}}, {{2, 1}, {1, 0}, {0, 3}}, {3}, {1}},
        // 5
        {{{"A", 0, 1, false}, {"B1", 0, 0, true}, {"B2", 1, 0, false}, {"C", 0, 2, false}}, {{1, 2}, {2, 0}, {0, 3}}, {3}, {0}},
        // 6
        {{{"A", 0, 0, true}, {"B1", 1, 1, false}, {"B2", 1, 0, true}, {"C", 0, 1, false}}, {{0, 3}, {0, 1}, {2, 3}, {2, 1}}, {3}, {1}},
        // 7
        {{{"A", 0, 0, true}, {"B1", 1, 1, false}, {"B2", 1, 0, true}, {"C", 0, 1, false}}, {{0, 3}, {2, 1}}, {3, 0}, {1, 2}},
        // 8
        {{{"A", 0, 0, false}, {"B1", 2, 1, true}, {"B2", 1, 1, false}, {"C", 0, 1, false}}, {{0, 3}}, {3, 2}, {1}},
    };
    return a;
}

// FM Drum algorithms 1..7 (operators A, B, C).
const std::vector<Algorithm>& fmDrumAlgorithms()
{
    static const std::vector<Algorithm> a = {
        {{{"A", 0, 0, false}, {"B", 1, 0, true}, {"C", 0, 1, false}}, {{0, 2}, {1, 2}}, {2}, {}},
        {{{"A", 0, 0, true}, {"B", 1, 0, true}, {"C", 0, 1, false}}, {{0, 2}, {1, 2}}, {2}, {}},
        {{{"A", 0, 0, false}, {"B", 1, 0, true}, {"C", 0, 1, false}}, {{1, 0}, {0, 2}, {1, 2}}, {2}, {}},
        {{{"A", 0, 0, false}, {"B", 1, 0, true}, {"C", 0, 1, false}}, {{1, 0}, {0, 2}}, {2}, {}},
        {{{"A", 0, 0, false}, {"B", 1, 0, true}, {"C", 0, 1, false}}, {{0, 2}}, {2, 1}, {}},
        {{{"A", 0, 0, true}, {"B", 1, 0, false}, {"C", 0, 1, false}}, {{0, 2}}, {2, 1}, {}},
        {{{"A", 0, 0, true}, {"B", 1, 0, true}, {"C", 0, 1, false}}, {{0, 2}}, {2, 1}, {}},
    };
    return a;
}

constexpr float kPitch = 1.6f; // distance between operator columns / rows, in box sizes

void arrowHead(juce::Graphics& g, juce::Point<float> tip, juce::Point<float> from, float size, float thickness)
{
    const auto d = tip - from;
    const float len = std::hypot(d.x, d.y);
    if (len <= 0.0f) return;
    const auto u = d / len, n = juce::Point<float>(-u.y, u.x);
    juce::Path p;
    p.startNewSubPath(tip - u * size + n * size * 0.6f);
    p.lineTo(tip);
    p.lineTo(tip - u * size - n * size * 0.6f);
    g.strokePath(p, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawAlgorithm(juce::Graphics& g, juce::Rectangle<float> area, const Algorithm& a, DiagramColours c, bool outputLabels)
{
    float cols = 0, rows = 0;
    for (const auto& o : a.ops) cols = std::max(cols, o.col + 1), rows = std::max(rows, o.row + 1);
    // in box units: feedback loops above / right, output bus and labels below
    const float w = (cols - 1) * kPitch + 1.0f + 0.55f, h = 0.32f + (rows - 1) * kPitch + 1.0f + (outputLabels ? 1.15f : 0.62f);
    const float u = std::min(area.getWidth() / w, area.getHeight() / h);
    const float ox = area.getCentreX() - w * u / 2.0f + 0.1f * u, oy = area.getCentreY() - h * u / 2.0f + 0.32f * u;
    const float stroke = std::max(1.2f, u * 0.075f);
    auto box = [&](const Op& o) { return juce::Rectangle<float>(ox + o.col * kPitch * u, oy + o.row * kPitch * u, u, u); };

    g.setColour(c.line);
    // modulation paths
    for (const auto& [from, to] : a.mods) {
        const auto s = box(a.ops[(size_t)from]), t = box(a.ops[(size_t)to]);
        juce::Point<float> p0, p1;
        if (a.ops[(size_t)from].row == a.ops[(size_t)to].row) { // side by side
            const bool right = t.getX() > s.getX();
            p0 = {right ? s.getRight() : s.getX(), s.getCentreY()};
            p1 = {right ? t.getX() : t.getRight(), t.getCentreY()};
        } else {
            p0 = {s.getCentreX(), s.getBottom()};
            p1 = {t.getCentreX(), t.getY()};
        }
        g.drawLine({p0, p1}, stroke);
        arrowHead(g, p1, p0, u * 0.16f, stroke);
    }
    // outputs: each carrier drops to its bus (X / Y); one under another leaves by its right side
    const float busY = oy + ((rows - 1) * kPitch + 1.0f + 0.4f) * u;
    auto exitX = [&](int i) {
        const auto& o = a.ops[(size_t)i];
        for (const auto& other : a.ops)
            if (other.col == o.col && other.row > o.row) return box(o).getRight() + 0.25f * u;
        return box(o).getCentreX();
    };
    auto bus = [&](const std::vector<int>& carriers, const char* label) {
        if (carriers.empty()) return;
        float lo = 1e9f, hi = -1e9f;
        for (int i : carriers) {
            const auto b = box(a.ops[(size_t)i]);
            const float x = exitX(i);
            if (x != b.getCentreX()) g.drawLine(b.getRight(), b.getCentreY(), x, b.getCentreY(), stroke);
            g.drawLine(x, x == b.getCentreX() ? b.getBottom() : b.getCentreY(), x, busY, stroke);
            lo = std::min(lo, x), hi = std::max(hi, x);
        }
        if (hi > lo) g.drawLine(lo, busY, hi, busY, stroke);
        const float lx = exitX(carriers.front());
        g.drawLine(lx, busY, lx, busY + 0.22f * u, stroke);
        if (label) {
            g.setColour(c.text);
            g.setFont(juce::Font(u * 0.42f));
            g.drawText(label, juce::Rectangle<float>(lx - u * 0.4f, busY + 0.24f * u, u * 0.8f, u * 0.5f), juce::Justification::centred);
            g.setColour(c.line);
        }
    };
    bus(a.x, outputLabels ? "X" : nullptr);
    bus(a.y, outputLabels ? "Y" : nullptr);
    // operators and feedback loops
    for (const auto& o : a.ops) {
        const auto b = box(o);
        g.setColour(c.line);
        g.drawRect(b.reduced(stroke / 2.0f), stroke);
        if (o.feedback) {
            // feedback: a square loop from the right side back into the top
            const float d = u * 0.22f, inY = b.getY() + u * 0.3f, inX = b.getRight() - u * 0.3f;
            juce::Path loop;
            loop.startNewSubPath(b.getRight(), inY);
            loop.lineTo(b.getRight() + d, inY);
            loop.lineTo(b.getRight() + d, b.getY() - d);
            loop.lineTo(inX, b.getY() - d);
            loop.lineTo(inX, b.getY());
            g.strokePath(loop, juce::PathStrokeType(stroke, juce::PathStrokeType::mitered, juce::PathStrokeType::square));
            arrowHead(g, {inX, b.getY()}, {inX, b.getY() - d}, u * 0.12f, stroke);
        }
        g.setColour(c.text);
        g.setFont(juce::Font(u * (std::strlen(o.name) > 1 ? 0.4f : 0.46f)));
        g.drawText(o.name, b, juce::Justification::centred);
    }
}

void sine(juce::Path& p, juce::Rectangle<float> r, float cycles)
{
    for (int i = 0; i <= 48; ++i) {
        const float t = (float)i / 48.0f;
        const juce::Point<float> pt(r.getX() + r.getWidth() * t, r.getCentreY() - std::sin(t * cycles * 6.2832f) * r.getHeight() / 2.0f);
        if (i == 0) p.startNewSubPath(pt);
        else p.lineTo(pt);
    }
}

} // namespace

void drawFmToneAlgorithm(juce::Graphics& g, juce::Rectangle<float> area, int algorithm, DiagramColours c)
{
    const auto& all = fmToneAlgorithms();
    drawAlgorithm(g, area, all[(size_t)juce::jlimit(0, (int)all.size() - 1, algorithm)], c, true);
}

void drawFmDrumAlgorithm(juce::Graphics& g, juce::Rectangle<float> area, int algorithm, DiagramColours c)
{
    const auto& all = fmDrumAlgorithms();
    drawAlgorithm(g, area, all[(size_t)juce::jlimit(0, (int)all.size() - 1, algorithm)], c, false);
}

void drawOscModulation(juce::Graphics& g, juce::Rectangle<float> area, int mode, DiagramColours c)
{
    // labels "1" / "2" on the left, the two oscillators as lanes, the modulation on the right
    const float h = std::min(area.getHeight(), area.getWidth() * 0.45f);
    auto r = area.withSizeKeepingCentre(std::min(area.getWidth(), h * 2.6f), h);
    const float stroke = std::max(1.2f, h * 0.035f), lane = h * 0.3f;
    auto labels = r.removeFromLeft(h * 0.3f);
    g.setColour(c.text);
    g.setFont(juce::Font(h * 0.28f));
    const float y1 = r.getY() + h * 0.25f, y2 = r.getY() + h * 0.75f;
    g.drawText("1", labels.withHeight(lane).withCentre({labels.getCentreX(), y1}), juce::Justification::centred);
    g.drawText("2", labels.withHeight(lane).withCentre({labels.getCentreX(), y2}), juce::Justification::centred);
    g.setColour(c.line);
    r.removeFromLeft(h * 0.08f);
    const auto stroked = juce::PathStrokeType(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    switch (juce::jlimit(0, 3, mode)) {
    case 0: { // off: two independent oscillators
        for (float y : {y1, y2}) {
            juce::Path p;
            sine(p, juce::Rectangle<float>(r.getX(), y - lane * 0.4f, r.getWidth(), lane * 0.8f), 2.0f);
            g.strokePath(p, stroked);
        }
        break;
    }
    case 1:
    case 2: { // ring modulation: 1 × 2, a fixed carrier frequency for the fixed variant
        const float d = h * 0.34f;
        const juce::Point<float> ring(r.getX() + r.getWidth() * 0.62f, r.getCentreY());
        for (float y : {y1, y2}) {
            juce::Path p;
            sine(p, juce::Rectangle<float>(r.getX(), y - lane * 0.3f, r.getWidth() * 0.28f, lane * 0.6f), 1.5f);
            p.lineTo(ring.x - d * 0.75f, y);
            p.lineTo(ring.x - d / 2.0f * 0.72f, ring.y + (y < ring.y ? -d * 0.36f : d * 0.36f));
            g.strokePath(p, stroked);
        }
        g.drawEllipse(juce::Rectangle<float>(d, d).withCentre(ring), stroke);
        const float k = d * 0.25f;
        g.drawLine(ring.x - k, ring.y - k, ring.x + k, ring.y + k, stroke);
        g.drawLine(ring.x - k, ring.y + k, ring.x + k, ring.y - k, stroke);
        const juce::Point<float> out(r.getRight(), ring.y);
        g.drawLine(ring.x + d / 2.0f, ring.y, out.x, out.y, stroke);
        arrowHead(g, out, {ring.x, ring.y}, h * 0.09f, stroke);
        if (mode == 2) {
            g.setColour(c.text);
            g.setFont(juce::Font(h * 0.2f));
            g.drawText("FIX", juce::Rectangle<float>(d * 1.4f, h * 0.22f).withCentre({ring.x, ring.y + d * 0.85f}), juce::Justification::centred);
        }
        break;
    }
    default: { // hard sync: 2 restarts on every period of 1
        const float x0 = r.getX(), w = r.getWidth(), period = w / 3.0f;
        juce::Path one, two;
        one.startNewSubPath(x0, y1 + lane * 0.35f);
        for (int i = 0; i < 3; ++i) {
            const float x = x0 + period * (float)i;
            one.lineTo(x, y1 - lane * 0.35f);
            one.lineTo(x + period / 2.0f, y1 - lane * 0.35f);
            one.lineTo(x + period / 2.0f, y1 + lane * 0.35f);
            one.lineTo(x + period, y1 + lane * 0.35f);
        }
        for (int i = 0; i < 3; ++i) { // 2 runs at 1.6 × the frequency, cut at each reset
            const float x = x0 + period * (float)i;
            two.startNewSubPath(x, y2 + lane * 0.35f);
            const float slope = period / 1.6f;
            two.lineTo(x + slope, y2 - lane * 0.35f);
            two.lineTo(x + slope, y2 + lane * 0.35f);
            two.lineTo(x + period, y2 + lane * 0.35f - (period - slope) / slope * lane * 0.7f);
            two.lineTo(x + period, y2 + lane * 0.35f);
        }
        g.strokePath(one, stroked);
        g.strokePath(two, stroked);
        g.setColour(c.line.withMultipliedAlpha(0.5f));
        for (int i = 1; i < 3; ++i) {
            const float x = x0 + period * (float)i;
            const float dashes[] = {stroke * 1.5f, stroke * 1.5f};
            g.drawDashedLine({x, y1 + lane * 0.4f, x, y2 - lane * 0.4f}, dashes, 2, stroke * 0.8f);
        }
        break;
    }
    }
}

void drawLogo(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto r = area.withSizeKeepingCentre(std::min(area.getWidth(), area.getHeight()), std::min(area.getWidth(), area.getHeight()));
    const float s = r.getWidth(), cx = r.getCentreX(), cy = r.getCentreY();
    g.setColour(colour);
    g.drawEllipse(r.reduced(s * 0.06f), s * 0.07f);
    const float x0 = cx - s * 0.2f, top = cy - s * 0.3f, bot = cy + s * 0.3f;
    juce::Path b;
    b.startNewSubPath(x0, top);
    b.lineTo(x0, bot);
    b.startNewSubPath(x0, top);
    b.quadraticTo(cx + s * 0.42f, (top + cy) / 2.0f, x0, cy);
    b.quadraticTo(cx + s * 0.5f, (cy + bot) / 2.0f, x0, bot);
    g.strokePath(b, juce::PathStrokeType(s * 0.085f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

} // namespace obui
