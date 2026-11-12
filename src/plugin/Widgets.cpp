#include "Widgets.h"

#include "Diagrams.h"

#include <cmath>
#include <map>


namespace obui {

using C = juce::Colour;

juce::Font uiFont(float size, bool bold)
{
    return juce::Font(juce::Font::getDefaultSansSerifFontName(), size, bold ? juce::Font::bold : juce::Font::plain);
}

namespace {
constexpr float kLabel = 15.0f; // parameter labels
constexpr float kValue = 16.5f; // values

void brackets(juce::Graphics& g, juce::Rectangle<float> b, float thickness = 1.3f)
{
    const float x0 = b.getX(), x1 = b.getRight(), y0 = b.getY(), y1 = b.getBottom(), w = 4.0f;
    g.drawLine(x0, y0, x0, y1, thickness);
    g.drawLine(x0, y0, x0 + w, y0, thickness);
    g.drawLine(x0, y1, x0 + w, y1, thickness);
    g.drawLine(x1, y0, x1, y1, thickness);
    g.drawLine(x1 - w, y0, x1, y0, thickness);
    g.drawLine(x1 - w, y1, x1, y1, thickness);
}

// "-----•--" position line under a value
void positionLine(juce::Graphics& g, juce::Rectangle<float> line, float n, juce::Colour c)
{
    g.setColour(c.withMultipliedAlpha(0.45f));
    g.fillRect(line.withHeight(2.0f));
    g.setColour(c);
    g.fillEllipse(line.getX() + line.getWidth() * n - 2.5f, line.getY() - 1.5f, 5.0f, 5.0f);
}
} // namespace

// ------------------------------------------------------------------------------------------ ParamControl
ParamControl::ParamControl(OpenBridgeProcessor& p, int param, int track) : proc_(p), param_(param), track_(track)
{
    const auto& d = def();
    setTooltip(juce::String(d.sectionName) + " " + d.name + (d.cc >= 0 ? "  (CC " + juce::String(d.cc) + ")" : ""));
    refresh();
}

const ob::ParamDef& ParamControl::def() const { return ob::digitone2Params()[(size_t)param_]; }

void ParamControl::refresh()
{
    const int r = proc_.model().get(param_, track_);
    const bool a = proc_.model().writable(param_, track_);
    if (r != raw_ || a != active_) {
        raw_ = r;
        active_ = a;
        repaint();
    }
}

juce::String ParamControl::valueText() const { return ob::formatValue(def(), raw_); }

float ParamControl::norm() const { return ob::normalisedFromRaw(def(), raw_); }

void ParamControl::setRaw(int raw)
{
    const auto& d = def();
    raw = juce::jlimit(d.min, d.max, raw);
    if (auto* p = proc_.parameterFor(param_, track_)) {
        if (!gesture_) p->beginChangeGesture();
        proc_.editorGesture = true;
        p->setValueNotifyingHost(ob::normalisedFromRaw(d, raw));
        proc_.editorGesture = false;
        if (!gesture_) p->endChangeGesture();
    } else {
        proc_.model().set(param_, track_, raw);
    }
    raw_ = proc_.model().get(param_, track_);
    repaint();
}

void ParamControl::mouseDown(const juce::MouseEvent&)
{
    dragStartNorm_ = norm();
    if (auto* p = proc_.parameterFor(param_, track_)) {
        p->beginChangeGesture();
        gesture_ = true;
    }
}

void ParamControl::mouseDrag(const juce::MouseEvent& e)
{
    const float speed = e.mods.isShiftDown() ? 0.0008f : 0.004f;
    const float n = juce::jlimit(0.0f, 1.0f, dragStartNorm_ - (float)e.getDistanceFromDragStartY() * speed);
    setRaw(ob::rawFromNormalised(def(), n));
}

void ParamControl::mouseUp(const juce::MouseEvent&)
{
    if (gesture_)
        if (auto* p = proc_.parameterFor(param_, track_)) p->endChangeGesture();
    gesture_ = false;
}

void ParamControl::mouseDoubleClick(const juce::MouseEvent&) { setRaw(def().def); }

void ParamControl::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto& d = def();
    const bool fine = d.decimals || d.format == ob::ParamFormat::Decimal || d.format == ob::ParamFormat::BipolarDecimal;
    const int step = (fine && e.mods.isShiftDown()) ? 1 : 256;
    setRaw(raw_ + (w.deltaY > 0 ? step : -step));
}

// ------------------------------------------------------------------------------------------ ValueBox
juce::String ValueBox::shownText() const
{
    if (ratioPart) return ob::ratioBPart(raw_, ratioPart == 2);
    return valueText();
}

void ValueBox::stepPart(int steps)
{
    // raw >> 1 = b1 * 19 + b2
    int i = raw_ >> 1, hi = i / 19, lo = i % 19;
    if (ratioPart == 1) lo = juce::jlimit(0, 18, lo + steps);
    else hi = juce::jlimit(0, 18, hi + steps);
    setRaw((hi * 19 + lo) * 2);
}

void ValueBox::mouseDown(const juce::MouseEvent& e)
{
    ParamControl::mouseDown(e);
    partStart_ = raw_;
}

void ValueBox::mouseDrag(const juce::MouseEvent& e)
{
    if (!ratioPart) return ParamControl::mouseDrag(e);
    const int i = partStart_ >> 1, steps = -e.getDistanceFromDragStartY() / 10;
    int hi = i / 19, lo = i % 19;
    if (ratioPart == 1) lo = juce::jlimit(0, 18, lo + steps);
    else hi = juce::jlimit(0, 18, hi + steps);
    if ((hi * 19 + lo) * 2 != raw_) setRaw((hi * 19 + lo) * 2);
}

void ValueBox::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (!ratioPart) return ParamControl::mouseWheelMove(e, w);
    stepPart(w.deltaY > 0 ? 1 : -1);
}

void ValueBox::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    if (boxed) {
        // FM ratio: square frame, title inside on top, value, position line
        g.setColour(C(tint->accent).withMultipliedAlpha(0.55f * alpha()));
        auto f = r.reduced(1.0f);
        const float gap = 22.0f;
        // frame open around the title
        g.drawLine(f.getX(), f.getY(), f.getCentreX() - gap, f.getY(), 1.6f);
        g.drawLine(f.getCentreX() + gap, f.getY(), f.getRight(), f.getY(), 1.6f);
        g.drawLine(f.getX(), f.getY(), f.getX(), f.getBottom(), 1.6f);
        g.drawLine(f.getRight(), f.getY(), f.getRight(), f.getBottom(), 1.6f);
        g.drawLine(f.getX(), f.getBottom(), f.getRight(), f.getBottom(), 1.6f);
        g.setColour(textColour());
        g.setFont(uiFont(kLabel));
        g.drawText(labelText(), f.withHeight(22).translated(0, -2), juce::Justification::centred);
        g.setColour(accent());
        g.setFont(uiFont(kValue + 1.0f));
        g.drawText(shownText(), f.withTrimmedTop(18).withHeight(32), juce::Justification::centred);
        float n = norm();
        if (ratioPart) n = (float)(ratioPart == 1 ? (raw_ >> 1) % 19 : (raw_ >> 1) / 19) / 18.0f;
        positionLine(g, {f.getCentreX() - 16, f.getY() + 54, 32, 2}, n, accent());
        return;
    }
    if (showLabel) {
        g.setColour(textColour());
        g.setFont(uiFont(kLabel));
        g.drawFittedText(labelText(), r.removeFromTop(22).toNearestInt(), juce::Justification::centred, 1, 0.75f);
    }
    const float w = juce::jlimit(30.0f, r.getWidth() - 4.0f, (float)uiFont(kValue).getStringWidth(valueText()) + 18.0f);
    auto box = r.removeFromTop(24).withSizeKeepingCentre(std::max(w, 34.0f), 22);
    g.setColour(C(tint->accent).withMultipliedAlpha(0.4f * alpha()));
    brackets(g, box);
    g.setColour(accent());
    g.setFont(uiFont(kValue));
    g.drawFittedText(valueText(), box.toNearestInt().reduced(2, 0), juce::Justification::centred, 1, 0.6f);
    positionLine(g, {box.getX() + 4, box.getBottom() + 4, box.getWidth() - 8, 2}, norm(), accent());
}

// ------------------------------------------------------------------------------------------ Knob
void Knob::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(textColour());
    g.setFont(uiFont(kLabel));
    if (label != "-") g.drawText(labelText(), r.removeFromTop(24), juce::Justification::centred);
    auto valueArea = r.removeFromBottom(22);
    const float d = std::min(std::min(r.getWidth(), r.getHeight()) - 4.0f, 60.0f);
    auto c = r.withSizeKeepingCentre(d, d);
    const auto centre = c.getCentre();
    const float rad = d * 0.5f - 6.0f;
    // outer dim ring, inner bright ring
    g.setColour(accent().withMultipliedAlpha(0.35f));
    g.drawEllipse(c.reduced(1.5f), 3.0f);
    g.setColour(accent());
    g.drawEllipse(juce::Rectangle<float>(rad * 2, rad * 2).withCentre(centre), 3.0f);
    const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
    const float a = a0 + (a1 - a0) * norm();
    const juce::Point<float> tip(centre.x + std::sin(a) * rad, centre.y - std::cos(a) * rad);
    g.drawLine(centre.x, centre.y, tip.x, tip.y, 2.5f);
    const juce::Point<float> dot(centre.x + std::sin(a) * (rad + 4.5f), centre.y - std::cos(a) * (rad + 4.5f));
    g.fillEllipse(dot.x - 3.0f, dot.y - 3.0f, 6.0f, 6.0f);
    g.setColour(textColour());
    g.setFont(uiFont(kLabel));
    g.drawText(valueText(), valueArea, juce::Justification::centred);
}

// ------------------------------------------------------------------------------------------ Toggle
void Toggle::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const bool on = style == Style::Inf ? raw_ >= 0x7F00 : raw_ > def().min;
    switch (style) {
    case Style::Hidden: return;
    case Style::Letter: {
        // mixer channel button ("M", "S"): square, filled when on
        auto b = r.reduced(1.0f);
        g.setColour(on ? C(onColour) : C(0xff3a3a3a));
        g.fillRect(b);
        g.setColour(on ? C(0xff1e1e1e) : C(Palette::text));
        g.setFont(uiFont(std::min(16.0f, b.getHeight() * 0.9f)));
        g.drawText(labelText(), b, juce::Justification::centred);
        return;
    }
    case Style::Inf:
    case Style::Switch: {
        // [grey][accent] when on, [light][dim] when off
        auto sw = r.withSizeKeepingCentre(26.0f, 12.0f);
        auto left = sw.removeFromLeft(13.0f), right = sw;
        const C dim = C(tint->accent).withMultipliedAlpha(0.35f * alpha());
        g.setColour(on ? dim : C(0xffb4b4b4).withMultipliedAlpha(alpha()));
        g.fillRect(left);
        g.setColour(on ? accent() : dim);
        g.fillRect(right);
        break;
    }
    case Style::Radio: {
        for (int i = 0; i < 2; ++i) {
            auto row = r.withHeight(r.getHeight() / 2).translated(0, i * r.getHeight() / 2);
            const bool sel = (i == 1) == on;
            g.setColour(sel ? accent() : accent().withMultipliedAlpha(0.3f));
            g.fillEllipse(row.removeFromLeft(14).withSizeKeepingCentre(10, 10));
            g.setColour(textColour());
            g.setFont(uiFont(14.0f));
            g.drawText(radioLabels[i], row.withTrimmedLeft(4), juce::Justification::centredLeft);
        }
        break;
    }
    case Style::Dot:
    default: {
        auto dot = r.removeFromLeft(14).withSizeKeepingCentre(10, 10);
        g.setColour(on ? accent() : accent().withMultipliedAlpha(0.3f));
        g.fillEllipse(dot);
        g.setColour(textColour().withMultipliedAlpha(on ? 1.0f : 0.6f));
        g.setFont(uiFont(14.0f));
        g.drawText(labelText(), r.withTrimmedLeft(4), juce::Justification::centredLeft);
    }
    }
}

void Toggle::mouseDown(const juce::MouseEvent& e)
{
    if (style == Style::Inf) {
        // the envelopes' "Inf": decay / release at their maximum (shown INF), back to the previous time
        if (raw_ >= 0x7F00) setRaw(restoreRaw);
        else {
            restoreRaw = raw_;
            setRaw(0x7F00);
        }
        return;
    }
    if (style == Style::Radio) setRaw(e.y >= getHeight() / 2 ? def().max : def().min);
    else setRaw(raw_ > def().min ? def().min : def().max);
}

// ------------------------------------------------------------------------------------------ SendIcon
void SendIcon::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(textColour());
    g.setFont(uiFont(kLabel));
    g.drawText(labelText(), r.removeFromTop(24), juce::Justification::centred);
    g.drawText(juce::String(raw_ >> 8), r.removeFromBottom(22), juce::Justification::centred); // prints 0, not OFF
    auto c = r.withSizeKeepingCentre(46, 46);
    const float n = norm();
    const C col = accent();
    if (icon == 0) {
        // delay: ring with a centre dot; echoes (rings) appear with the send level
        g.setColour(col);
        g.drawEllipse(c.reduced(3), 2.6f);
        g.drawEllipse(c.withSizeKeepingCentre(9, 9), 2.0f);
        const int echoes = (int)std::ceil(n * 3.0f);
        for (int i = 1; i <= echoes; ++i) {
            const float d = 9.0f + (c.getWidth() - 15.0f) * (float)i / 4.0f;
            g.setColour(col.withMultipliedAlpha(0.6f - 0.12f * (float)i));
            g.drawEllipse(c.withSizeKeepingCentre(d, d), 1.4f);
        }
    } else if (icon == 1) {
        // reverb: wire cube filled like a volume (front, side and top faces) up to the send level
        const float s = 30, o = 11;
        auto front = juce::Rectangle<float>(c.getX() + 1, c.getY() + o + 2, s, s);
        const float h = s * n, yTop = front.getBottom() - h;
        juce::Path vol;
        vol.startNewSubPath(front.getX(), front.getBottom());
        vol.lineTo(front.getRight(), front.getBottom());
        vol.lineTo(front.getRight() + o, front.getBottom() - o);
        vol.lineTo(front.getRight() + o, yTop - o);
        vol.lineTo(front.getX() + o, yTop - o);
        vol.lineTo(front.getX(), yTop);
        vol.closeSubPath();
        g.setColour(col.withMultipliedAlpha(0.35f));
        g.fillPath(vol);
        g.setColour(col.withMultipliedAlpha(0.55f));
        g.fillRect(front.withTop(yTop)); // front face, brighter
        if (n > 0.01f) {                 // surface of the "liquid"
            juce::Path top;
            top.addQuadrilateral(front.getX(), yTop, front.getRight(), yTop, front.getRight() + o, yTop - o, front.getX() + o, yTop - o);
            g.setColour(col.withMultipliedAlpha(0.75f));
            g.fillPath(top);
        }
        g.setColour(col);
        g.drawRect(front, 1.8f);
        g.drawRect(front.translated(o, -o), 1.4f);
        for (auto p : {front.getTopLeft(), front.getTopRight(), front.getBottomLeft(), front.getBottomRight()})
            g.drawLine(p.x, p.y, p.x + o, p.y - o, 1.4f);
    } else {
        // chorus: cone filled from its base up to the send level
        const float apexY = c.getY() + 2, baseY = c.getBottom() - 8;
        juce::Path cone;
        cone.startNewSubPath(c.getCentreX(), apexY);
        cone.lineTo(c.getRight() - 2, baseY);
        cone.lineTo(c.getX() + 2, baseY);
        cone.closeSubPath();
        auto base = juce::Rectangle<float>(c.getX() + 2, baseY - 6, c.getWidth() - 4, 12);
        {
            juce::Graphics::ScopedSaveState state(g);
            const float level = baseY - (baseY - apexY) * n;
            g.reduceClipRegion(juce::Rectangle<float>(c.getX(), level, c.getWidth(), c.getBottom() - level).toNearestInt());
            g.setColour(col.withMultipliedAlpha(0.5f));
            g.fillPath(cone);
            g.fillEllipse(base);
        }
        g.setColour(col);
        g.strokePath(cone, juce::PathStrokeType(1.6f));
        g.drawEllipse(base, 1.4f);
    }
}

// ------------------------------------------------------------------------------------------ Fader
void Fader::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(textColour());
    g.setFont(uiFont(kLabel));
    if (label != "-") g.drawText(labelText(), r.removeFromTop(24), juce::Justification::centred);
    if (showValue) g.drawText(valueText(), r.removeFromBottom(22), juce::Justification::centred);
    auto track = r.withSizeKeepingCentre(11, r.getHeight() - (showValue ? 10.0f : 2.0f));
    g.setColour(accent().withMultipliedAlpha(0.3f));
    g.fillRect(track);
    auto fill = track.withTop(track.getBottom() - track.getHeight() * norm());
    g.setColour(accent());
    g.fillRect(fill);
    g.setColour(C(0xfff0f0f0));
    g.fillRect(track.getX(), fill.getY() - 2.0f, track.getWidth(), 4.0f);
}

void Fader::mouseDrag(const juce::MouseEvent& e)
{
    const float h = (float)getHeight() - 56;
    const float n = juce::jlimit(0.0f, 1.0f, dragStartNorm_ - (float)e.getDistanceFromDragStartY() / h);
    setRaw(ob::rawFromNormalised(def(), n));
}

// ------------------------------------------------------------------------------------------ Bar
void Bar::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float n = norm();
    if (vertical) {
        auto valueArea = r.removeFromBottom(36);
        g.setColour(textColour());
        g.setFont(uiFont(kLabel));
        g.drawText(labelText(), r.removeFromBottom(26), juce::Justification::centred);
        auto col = r.withSizeKeepingCentre(10, r.getHeight() - 8);
        g.setColour(accent().withMultipliedAlpha(0.35f));
        g.fillRect(col);
        g.setColour(accent());
        g.fillRect(col.withTop(col.getBottom() - col.getHeight() * n));
        auto box = valueArea.removeFromTop(24).withSizeKeepingCentre(40, 22);
        g.setColour(accent().withMultipliedAlpha(0.4f));
        brackets(g, box);
        g.setColour(accent());
        g.setFont(uiFont(kValue));
        g.drawText(valueText(), box, juce::Justification::centred);
        positionLine(g, {box.getX() + 4, box.getBottom() + 4, box.getWidth() - 8, 2}, n, accent());
        return;
    }
    if (compact) {
        // mixer pan: thin bar with a marker
        auto bar = r.withSizeKeepingCentre(r.getWidth() - 4, 6);
        g.setColour(C(0xff3a3a3a));
        g.fillRect(bar);
        g.setColour(C(0xffdf575e).withMultipliedAlpha(0.8f));
        g.fillRect(bar.getX(), bar.getY() - 2, bar.getWidth(), 1.5f);
        g.setColour(C(0xfff0f0f0).withMultipliedAlpha(alpha()));
        g.fillRect(bar.getX() + bar.getWidth() * n - 2, bar.getY() - 1, 4.0f, bar.getHeight() + 2);
        return;
    }
    if (valueRight) {
        // setup page: "[====      ]  2"
        auto bar = r.withWidth(r.getWidth() - 50).withSizeKeepingCentre(r.getWidth() - 50, 12);
        g.setColour(C(0xff3a3a3a));
        g.fillRect(bar);
        g.setColour(C(0xfff0f0f0).withMultipliedAlpha(alpha()));
        g.fillRect(bar.withWidth(std::max(6.0f, bar.getWidth() * n)));
        g.setColour(textColour());
        g.setFont(uiFont(kValue));
        g.drawText(valueText(), r.removeFromRight(40), juce::Justification::centredLeft);
        return;
    }
    g.setColour(textColour());
    g.setFont(uiFont(kLabel));
    g.drawText(labelText(), r.removeFromTop(24), juce::Justification::centred);
    auto bar = r.removeFromTop(14).reduced(4, 1);
    g.setColour(accent().withMultipliedAlpha(0.35f));
    g.fillRect(bar);
    const float segW = bar.getWidth() * 0.3f;
    g.setColour(accent());
    g.fillRect(juce::Rectangle<float>(bar.getX() + (bar.getWidth() - segW) * n, bar.getY(), segW, bar.getHeight()));
    auto box = r.withTrimmedTop(8).removeFromTop(24).withSizeKeepingCentre(40, 22);
    g.setColour(accent().withMultipliedAlpha(0.4f));
    brackets(g, box);
    g.setColour(accent());
    g.setFont(uiFont(kValue));
    g.drawText(valueText(), box, juce::Justification::centred);
    positionLine(g, {box.getX() + 4, box.getBottom() + 4, box.getWidth() - 8, 2}, n, accent());
}

void Bar::mouseDown(const juce::MouseEvent& e)
{
    ParamControl::mouseDown(e);
    if (!vertical) {
        const float n = juce::jlimit(0.0f, 1.0f, (float)(e.x - 4) / (float)(getWidth() - (valueRight ? 58 : 8)));
        setRaw(ob::rawFromNormalised(def(), n));
        dragStartNorm_ = norm();
    }
}

void Bar::mouseDrag(const juce::MouseEvent& e)
{
    if (vertical) {
        const float h = (float)getHeight() - 70;
        setRaw(ob::rawFromNormalised(def(), juce::jlimit(0.0f, 1.0f, dragStartNorm_ - (float)e.getDistanceFromDragStartY() / h)));
    } else {
        const float n = juce::jlimit(0.0f, 1.0f, (float)(e.x - 4) / (float)(getWidth() - (valueRight ? 58 : 8)));
        setRaw(ob::rawFromNormalised(def(), n));
    }
}

// ------------------------------------------------------------------------------------------ Choice
int Choice::selected() const { return juce::jlimit(0, std::max(0, options.size() - 1), raw_ >> 8); }

namespace {
void waveGlyph(juce::Graphics& g, int wave, juce::Rectangle<float> r)
{
    juce::Path p;
    const float x0 = r.getX(), x1 = r.getRight(), y0 = r.getY(), y1 = r.getBottom(), xm = r.getCentreX(), ym = r.getCentreY();
    switch (wave) {
    case 0: p.startNewSubPath(x0, ym); p.lineTo(x0 + r.getWidth() * 0.25f, y0); p.lineTo(x0 + r.getWidth() * 0.75f, y1); p.lineTo(x1, ym); break; // triangle
    case 1:
        p.startNewSubPath(x0, ym);
        for (int i = 1; i <= 20; ++i) p.lineTo(x0 + r.getWidth() * i / 20.0f, ym - std::sin(i / 20.0f * 6.2832f) * r.getHeight() * 0.5f);
        break; // sine
    case 2: p.startNewSubPath(x0, y1); p.lineTo(x0, y0); p.lineTo(xm, y0); p.lineTo(xm, y1); p.lineTo(x1, y1); p.lineTo(x1, y0); break; // square
    case 3: p.startNewSubPath(x0, y1); p.lineTo(x0, y0); p.lineTo(x1, y1); p.lineTo(x1, y0); break;                                    // saw
    case 4: p.startNewSubPath(x0, y0); p.quadraticTo(x0 + 2, y1, x1, y1); break;                                                        // exponential
    case 5: p.startNewSubPath(x0, y1); p.lineTo(x1, y0); p.lineTo(x1, y1); break;                                                       // ramp
    default:
        p.startNewSubPath(x0, ym); p.lineTo(x0 + 3, ym); p.lineTo(x0 + 3, y0); p.lineTo(xm, y0); p.lineTo(xm, y1);
        p.lineTo(x1 - 3, y1); p.lineTo(x1 - 3, ym + 2); p.lineTo(x1, ym + 2); // random
    }
    g.strokePath(p, juce::PathStrokeType(1.7f));
}
} // namespace

void Choice::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const int sel = selected();
    if (bigValue) {
        g.setColour(textColour());
        g.setFont(uiFont(kValue));
        // the first option (phase reset "OFF") stays visible, bright when selected
        g.setColour(sel < firstInRow ? textColour() : C(tint->title).withMultipliedAlpha(0.45f * alpha()));
        g.drawText(options[0], r.removeFromTop(r.getHeight() * 0.5f), juce::Justification::centredBottom);
    }
    const int n = options.size() - firstInRow;
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) {
        const int opt = (i < (int)order.size() ? order[(size_t)i] : i) + firstInRow;
        g.setColour(opt == sel ? C(0xfff7f7f7).withMultipliedAlpha(alpha()) : C(tint->title).withMultipliedAlpha(0.45f * alpha()));
        const auto cell = cellBounds(i, n, r);
        if (waveIcons) waveGlyph(g, opt, cell.withSizeKeepingCentre(std::min(cell.getWidth() - 8.0f, 22.0f), 16.0f));
        else {
            g.setFont(uiFont(kLabel, opt == sel && options.size() <= 8 && !bigValue && options[0] == "1"));
            g.drawText(options[opt], cell, columns > 0 ? juce::Justification::centredLeft : juce::Justification::centred);
        }
    }
}

juce::Rectangle<float> Choice::cellBounds(int i, int n, juce::Rectangle<float> r) const
{
    if (columns > 0) {
        const int rows = (n + columns - 1) / columns;
        const float w = r.getWidth() / (float)columns, h = r.getHeight() / (float)rows;
        return {r.getX() + (float)(i % columns) * w, r.getY() + (float)(i / columns) * h, w, h};
    }
    if (vertical) {
        const float h = r.getHeight() / (float)n;
        return {r.getX(), r.getY() + (float)i * h, r.getWidth(), h};
    }
    const float w = r.getWidth() / (float)n;
    return {r.getX() + (float)i * w, r.getY(), w, r.getHeight()};
}

void Choice::mouseDown(const juce::MouseEvent& e)
{
    const int n = options.size() - firstInRow;
    if (n <= 0) return;
    auto r = getLocalBounds().toFloat();
    if (bigValue) {
        if (e.y < getHeight() / 2) {
            setRaw(0); // the large value ("OFF")
            return;
        }
        r.removeFromTop(r.getHeight() * 0.5f);
    }
    for (int i = 0; i < n; ++i)
        if (cellBounds(i, n, r).contains(e.position)) {
            setRaw(((i < (int)order.size() ? order[(size_t)i] : i) + firstInRow) << 8);
            return;
        }
}

// ------------------------------------------------------------------------------------------ ChoiceBox
ChoiceBox::ChoiceBox(OpenBridgeProcessor& p, int param, int track) : ParamControl(p, param, track)
{
    addAndMakeVisible(box_);
    box_.onChange = [this] {
        const int i = box_.getSelectedItemIndex();
        if (i >= 0 && i < (int)values_.size() && values_[(size_t)i] != raw_) setRaw(values_[(size_t)i]);
    };
}

void ChoiceBox::setOptions(const juce::StringArray& names, const std::vector<int>& values)
{
    box_.clear(juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i) box_.addItem(names[i], i + 1);
    values_ = values;
    shown_ = -1;
    refreshSelection();
}

void ChoiceBox::refreshSelection()
{
    if (shown_ == raw_) return;
    shown_ = raw_;
    int best = -1;
    for (size_t i = 0; i < values_.size(); ++i)
        if (values_[i] == raw_) best = (int)i;
    box_.setSelectedItemIndex(best, juce::dontSendNotification);
    if (best < 0) box_.setText(valueText(), juce::dontSendNotification);
}

void ChoiceBox::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop(30);
    box_.setBounds(r.removeFromTop(34));
}

void ChoiceBox::paint(juce::Graphics& g)
{
    refreshSelection();
    g.setColour(textColour());
    g.setFont(uiFont(kLabel));
    g.drawText(labelText(), getLocalBounds().removeFromTop(26), juce::Justification::centred);
}

// ------------------------------------------------------------------------------------------ Graph
Graph::Graph(OpenBridgeProcessor& p, int track, Kind kind, int index) : proc_(p), track_(track), kind_(kind), index_(index)
{
    std::vector<const char*> keys;
    switch (kind) {
    case Kind::FilterEnvelope:
        keys = {"Sound_FilterEnvDelay", "Sound_FilterAttack", "Sound_FilterDecay", "Sound_FilterSustain", "Sound_FilterRelease",
                "Sound_FilterEnvDepth"};
        break;
    case Kind::AmpEnvelope:
        keys = {"Sound_AmpDelayTime", "Sound_AmpAttack", "Sound_AmpHold", "Sound_AmpDecay", "Sound_AmpSustain", "Sound_AmpRelease",
                "Sound_AmpMode"};
        break;
    case Kind::BaseWidth: keys = {"Sound_FilterBase", "Sound_FilterWidth"}; break;
    case Kind::FilterResponse:
        keys = {"Sound_MultiModeFilterFrequency", "Sound_MultiModeFilterResonance", "Sound_Lp4FilterFrequency", "Sound_Lp4FilterResonance",
                "Sound_LegacyFilterFrequency", "Sound_LegacyFilterResonance", "Sound_EqFilterFrequency", "Sound_EqFilterGain",
                "Sound_CombFilterFrequency", "Sound_CombPlusFilterFrequency", "Sound_CombFilterFeedback", "Sound_CombPlusFilterFeedback"};
        break;
    case Kind::FmEnvA: keys = {"Sound_FmADelay", "Sound_FmAAttack", "Sound_FmADecay", "Sound_FmAEnd", "Sound_FmALevel"}; break;
    case Kind::FmEnvB: keys = {"Sound_FmBDelay", "Sound_FmBAttack", "Sound_FmBDecay", "Sound_FmBEnd", "Sound_FmBLevel"}; break;
    case Kind::Harmonics: keys = {"Sound_FmHarmonics"}; break;
    case Kind::Detune: keys = {"Sound_FmDetune"}; break;
    case Kind::Feedback: keys = {"Sound_FmFeedback"}; break;
    case Kind::Lfo: keys = {"Sound_Lfo1Waveform", "Sound_Lfo2Waveform", "Sound_Lfo3Waveform", "Sound_Lfo1Start", "Sound_Lfo2Start", "Sound_Lfo3Start"}; break;
    case Kind::Algorithm: keys = {"Sound_FmAlgorithm"}; break;
    case Kind::DrumAlgorithm: keys = {"Sound_DrumAlgo"}; break;
    case Kind::OscMod: keys = {"Sound_VaOscMod"}; break;
    }
    for (auto* k : keys)
        if (const auto* d = ob::findParam(k)) params_.push_back((int)(d - ob::digitone2Params().data()));
    setInterceptsMouseClicks(false, false);
    startTimerHz(15);
}

Graph::Graph(OpenBridgeProcessor& p, int track, Kind kind, std::vector<std::string> keys)
    : proc_(p), track_(track), kind_(kind), index_(0), keys_(std::move(keys))
{
    for (auto& k : keys_)
        if (const auto* d = ob::findParam(k)) params_.push_back((int)(d - ob::digitone2Params().data()));
    setInterceptsMouseClicks(false, false);
    startTimerHz(15);
}

float Graph::value(const char* key) const
{
    const auto* d = ob::findParam(key);
    if (!d) return 0;
    const int i = (int)(d - ob::digitone2Params().data());
    return ob::normalisedFromRaw(*d, proc_.model().get(i, track_));
}

int Graph::raw(const char* key) const
{
    const auto* d = ob::findParam(key);
    return d ? proc_.model().get((int)(d - ob::digitone2Params().data()), track_) : 0;
}

void Graph::timerCallback()
{
    std::vector<int> now;
    for (int p : params_) now.push_back(proc_.model().get(p, track_));
    now.push_back(proc_.model().filterMachine(track_));
    if (now != last_) {
        last_ = now;
        repaint();
    }
}

void Graph::paint(juce::Graphics& g)
{
    if (fill) {
        g.setColour(C(tint->group));
        g.fillRect(getLocalBounds());
    }
    auto r = getLocalBounds().toFloat().reduced(10, 12);
    const C colour = C(tint->accent);
    const C dimLine = colour.withMultipliedAlpha(0.45f);
    juce::Path path;
    std::vector<juce::Point<float>> dots;
    switch (kind_) {
    case Kind::BaseWidth: {
        const float base = value("Sound_FilterBase"), width = value("Sound_FilterWidth");
        const float x0 = r.getX() + r.getWidth() * 0.15f + base * r.getWidth() * 0.35f;
        const float x1 = std::min(r.getRight() - 20, x0 + 20 + width * r.getWidth() * 0.45f);
        path.startNewSubPath(x0 - r.getWidth() * 0.12f, r.getBottom());
        path.lineTo(x0, r.getY() + 4);
        path.lineTo(x1, r.getY() + 4);
        path.lineTo(x1 + r.getWidth() * 0.12f, r.getBottom());
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.2f));
        g.setColour(colour);
        for (auto p : {juce::Point<float>{x0, r.getY() + 4}, juce::Point<float>{x1, r.getY() + 4}}) g.fillEllipse(p.x - 4.5f, p.y - 4.5f, 9, 9);
        return;
    }
    case Kind::FilterResponse: {
        const int f = proc_.model().filterMachine(track_);
        const char* fk[] = {"Sound_MultiModeFilterFrequency", "Sound_Lp4FilterFrequency", "Sound_EqFilterFrequency", "Sound_CombFilterFrequency",
                            "Sound_LegacyFilterFrequency", "Sound_CombPlusFilterFrequency"};
        const char* rk[] = {"Sound_MultiModeFilterResonance", "Sound_Lp4FilterResonance", "Sound_EqFilterGain", "Sound_CombFilterFeedback",
                            "Sound_LegacyFilterResonance", "Sound_CombPlusFilterFeedback"};
        const int fi = juce::jlimit(0, 5, f);
        const float freq = value(fk[fi]), reso = value(rk[fi]);
        const float fc = r.getX() + r.getWidth() * (0.1f + 0.8f * freq);
        const float flat = r.getY() + r.getHeight() * 0.55f;
        if (fi == 3 || fi == 5) { // comb: evenly spaced peaks
            path.startNewSubPath(r.getX(), flat);
            const float spacing = 12.0f + 40.0f * (1.0f - freq);
            for (float x = r.getX(); x < r.getRight(); x += 1.0f) {
                const float ph = std::fmod(x - r.getX(), spacing) / spacing;
                path.lineTo(x, flat - (reso * 0.8f + 0.1f) * r.getHeight() * 0.45f * std::pow(std::cos(ph * 3.14159f), 8.0f));
            }
        } else if (fi == 2) { // equalizer bell
            const float gain = (reso - 0.5f) * 2.0f;
            path.startNewSubPath(r.getX(), flat);
            for (float x = r.getX(); x <= r.getRight(); x += 2.0f) {
                const float d = (x - fc) / (r.getWidth() * 0.12f);
                path.lineTo(x, flat - gain * r.getHeight() * 0.4f * std::exp(-d * d));
            }
        } else { // low-pass with a resonance peak
            const float peak = r.getHeight() * 0.5f * reso;
            path.startNewSubPath(r.getX(), flat);
            path.lineTo(fc - 50.0f, flat);
            path.cubicTo(fc - 15.0f, flat, fc - 6.0f, flat - peak - 4.0f, fc, flat - peak - 6.0f);
            path.cubicTo(fc + 8.0f, flat - peak, fc + 18.0f, r.getBottom() - 10.0f, fc + 40.0f, r.getBottom() + 12.0f);
        }
        g.setColour(colour);
        g.strokePath(path, juce::PathStrokeType(2.4f));
        return;
    }
    case Kind::FmEnvA:
    case Kind::FmEnvB: {
        const bool a = kind_ == Kind::FmEnvA;
        const float del = value(a ? "Sound_FmADelay" : "Sound_FmBDelay"), atk = value(a ? "Sound_FmAAttack" : "Sound_FmBAttack");
        const float dec = value(a ? "Sound_FmADecay" : "Sound_FmBDecay"), end = value(a ? "Sound_FmAEnd" : "Sound_FmBEnd");
        const float lev = value(a ? "Sound_FmALevel" : "Sound_FmBLevel");
        // Rise to the top (attack), a short reset segment, then the decay to the end level.
        auto area = r.withTrimmedRight(36);
        const float top = area.getY() + 4, bottom = area.getBottom(), W = area.getWidth();
        const float x0 = area.getX() + del * W * 0.3f;
        path.startNewSubPath(area.getX(), bottom);
        path.lineTo(x0, bottom);
        const float xa = x0 + atk * W * 0.3f;
        path.lineTo(xa, top);
        dots.push_back({xa, top});
        const float xr = xa + W * 0.055f;
        path.lineTo(xr, top);
        dots.push_back({xr, top});
        const float endY = bottom - end * (bottom - top);
        const float xd = std::min(area.getRight() - 4, xr + W * (0.04f + 1.05f * dec));
        path.cubicTo(xr + (xd - xr) * 0.15f, endY, xr + (xd - xr) * 0.4f, endY, xd, endY);
        dots.push_back({xd, endY});
        path.lineTo(area.getRight(), endY);
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.2f));
        g.setColour(colour);
        for (auto p : dots) g.fillEllipse(p.x - 4.5f, p.y - 4.5f, 9, 9);
        auto bar = juce::Rectangle<float>(r.getRight() - 12, r.getY(), 11, r.getHeight());
        g.setColour(colour.withMultipliedAlpha(0.18f));
        g.fillRect(bar);
        g.setColour(colour);
        g.fillRect(bar.withTop(bar.getBottom() - bar.getHeight() * lev));
        return;
    }
    case Kind::Harmonics: {
        const float h = value("Sound_FmHarmonics");
        // one period of the carrier, centred, with the harmonics added as the value moves from the centre
        const float w = std::min(r.getWidth(), r.getHeight() * 2.1f), x0 = r.getCentreX() - w / 2, amp = r.getHeight() * 0.45f;
        const float k = std::abs(h - 0.5f) * 2.0f, n = h < 0.5f ? 2.0f + (0.5f - h) * 8.0f : 3.0f + (h - 0.5f) * 8.0f;
        for (int i = 0; i <= 80; ++i) {
            const float t = (float)i / 80.0f;
            const float y = (std::sin(t * 6.2832f) * (1.0f - 0.4f * k) + std::sin(t * 6.2832f * n) * 0.4f * k);
            const float px = x0 + w * t, py = r.getCentreY() - y * amp;
            if (i == 0) path.startNewSubPath(px, py);
            else path.lineTo(px, py);
        }
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.0f));
        return;
    }
    case Kind::Detune: {
        const float d = value("Sound_FmDetune");
        g.setColour(dimLine);
        for (int i = 0; i < 3; ++i) {
            juce::Path p;
            const float y = r.getY() + r.getHeight() * (0.2f + 0.3f * i);
            const float bend = (i - 1) * d * r.getHeight() * 0.25f;
            p.startNewSubPath(r.getX(), y);
            p.quadraticTo(r.getCentreX(), y - bend, r.getRight(), y);
            g.strokePath(p, juce::PathStrokeType(2.0f));
        }
        return;
    }
    case Kind::Feedback: {
        const float fb = value("Sound_FmFeedback");
        g.setColour(dimLine);
        const int n = 2 + (int)(fb * 4);
        const float size = std::min(r.getWidth(), r.getHeight()) * 0.62f;
        for (int i = 0; i < n; ++i) {
            const float s = 1.0f - (float)i / (float)n;
            juce::Path p;
            const float w = size * s, hh = size * s;
            const auto c = r.getCentre();
            p.addQuadrilateral(c.x - w, c.y, c.x, c.y - hh, c.x + w, c.y, c.x, c.y + hh);
            g.strokePath(p, juce::PathStrokeType(2.0f));
        }
        return;
    }
    case Kind::Lfo: {
        const char* wk[] = {"Sound_Lfo1Waveform", "Sound_Lfo2Waveform", "Sound_Lfo3Waveform"};
        const int wave = juce::jlimit(0, 6, raw(wk[juce::jlimit(0, 2, index_)]) >> 8);
        auto area = r.reduced(4, 6);
        g.setColour(C(0xff6c686d));
        // one period of the waveform, from the start phase
        const char* sk[] = {"Sound_Lfo1Start", "Sound_Lfo2Start", "Sound_Lfo3Start"};
        const float phase0 = value(sk[juce::jlimit(0, 2, index_)]);
        juce::Path p;
        const int steps = 120;
        uint32_t seed = 12345;
        float rnd = 0;
        for (int i = 0; i <= steps; ++i) {
            const float t = (float)i / steps + phase0, ph = t - std::floor(t);
            float y;
            switch (wave) {
            case 0: y = ph < 0.25f ? ph * 4 : ph < 0.75f ? 2 - ph * 4 : ph * 4 - 4; break;
            case 1: y = std::sin(ph * 6.2832f); break;
            case 2: y = ph < 0.5f ? 1.0f : -1.0f; break;
            case 3: y = 1.0f - 2.0f * ph; break;
            case 4: y = 2.0f * std::exp(-ph * 4.0f) - 1.0f; break;
            case 5: y = 2.0f * ph - 1.0f; break;
            default:
                if (i % 8 == 0) {
                    seed = seed * 1103515245u + 12345u;
                    rnd = (float)((seed >> 16) & 0xff) / 127.5f - 1.0f;
                }
                y = rnd;
            }
            const float px = area.getX() + area.getWidth() * (float)i / steps, py = area.getCentreY() - y * area.getHeight() * 0.6f;
            if (i == 0) p.startNewSubPath(px, py);
            else p.lineTo(px, py);
        }
        g.strokePath(p, juce::PathStrokeType(1.6f));
        return;
    }
    case Kind::Algorithm: {
        // operator diagram of the selected algorithm
        const int algo = juce::jlimit(0, 7, raw("Sound_FmAlgorithm") >> 8);
        drawFmToneAlgorithm(g, getLocalBounds().toFloat().reduced(2.0f), algo, {colour.withMultipliedAlpha(0.42f), colour.withMultipliedAlpha(0.85f)});
        return;
    }
    case Kind::CrossIcon: {
        // WaveTone "Linear Offset": cross whose vertical line follows the offset
        const float v = keys_.empty() ? 0.5f : value(keys_[0].c_str());
        g.setColour(dimLine);
        g.drawLine(r.getX(), r.getY() + 4, r.getRight(), r.getY() + 4, 2.0f);
        g.drawLine(r.getX(), r.getBottom() - 4, r.getRight(), r.getBottom() - 4, 2.0f);
        const float x = r.getX() + r.getWidth() * v;
        g.drawLine(x, r.getY() + 10, x, r.getBottom() - 10, 2.0f);
        return;
    }
    case Kind::TableIcon: {
        g.setColour(dimLine);
        g.drawLine(r.getX(), r.getY() + 4, r.getRight(), r.getY() + 4, 2.0f);
        g.drawLine(r.getX(), r.getBottom() - 4, r.getRight(), r.getBottom() - 4, 2.0f);
        g.setColour(C(0xffa9b9b6));
        g.setFont(uiFont(18.0f));
        g.drawText((raw(keys_[0].c_str()) >> 8) ? "Harm." : "Prim.", r, juce::Justification::centred);
        return;
    }
    case Kind::DriftIcon: {
        const float v = keys_.empty() ? 0.0f : value(keys_[0].c_str());
        g.setColour(dimLine);
        g.drawLine(r.getX(), r.getBottom() - 4, r.getRight(), r.getBottom() - 4, 2.0f);
        g.setColour(colour.withMultipliedAlpha(0.8f));
        const int dots = 1 + (int)(v * 7.0f);
        for (int i = 0; i < dots; ++i) g.fillEllipse(r.getCentreX() - 3, r.getBottom() - 12 - (float)i * 9.0f, 6, 6);
        return;
    }
    case Kind::CompCurve: {
        // compressor transfer curve: 1:1 below the threshold, flatter above it (by the ratio)
        const float thr = value(keys_[0].c_str()), ratio = keys_.size() > 1 ? value(keys_[1].c_str()) : 0.5f;
        const float kx = r.getX() + r.getWidth() * (0.15f + 0.6f * thr), ky = r.getBottom() - (kx - r.getX()) * r.getHeight() / r.getWidth();
        const float slope = 1.0f / (1.5f + ratio * 18.0f);
        path.startNewSubPath(r.getX(), r.getBottom());
        path.lineTo(kx, ky);
        path.lineTo(r.getRight(), ky - (r.getRight() - kx) * slope * r.getHeight() / r.getWidth());
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.0f));
        g.setColour(colour);
        g.fillEllipse(kx - 4.5f, ky - 4.5f, 9, 9);
        return;
    }
    case Kind::DelayEcho: {
        // delay: L / R axis and the echoes (spacing by time, size by feedback, offset by width)
        const float t = value(keys_[0].c_str()), fb = value(keys_[1].c_str()), w = value(keys_[2].c_str());
        const float axisX = r.getX() + 4, cy = r.getCentreY();
        g.setColour(dimLine);
        g.drawLine(axisX, r.getY(), axisX, r.getBottom(), 2.0f);
        g.drawLine(axisX, cy, r.getRight(), cy, 1.5f);
        g.setFont(uiFont(13.0f));
        g.drawText("L", juce::Rectangle<float>(axisX - 2, r.getY() - 2, 14, 14), juce::Justification::centred);
        g.drawText("R", juce::Rectangle<float>(axisX - 2, r.getBottom() - 12, 14, 14), juce::Justification::centred);
        const float step = 18.0f + t * 60.0f;
        float size = 16.0f, x = axisX + 30.0f;
        for (int i = 0; i < 12 && x < r.getRight(); ++i) {
            const float off = (i % 2 ? 1.0f : -1.0f) * (w - 0.5f) * r.getHeight() * 0.5f;
            if (size > 3.0f) {
                g.setColour(colour);
                g.fillEllipse(x - size / 2, cy + off - size / 2, size, size);
            } else {
                g.setColour(dimLine);
                g.drawLine(x, cy - r.getHeight() * 0.15f, x, cy + r.getHeight() * 0.15f, 1.5f);
            }
            x += step;
            size *= 0.35f + 0.6f * fb;
        }
        return;
    }
    case Kind::ReverbSpace: {
        // reverb: circle with axes and a cube sized by the decay
        const float dec = value(keys_[0].c_str()), pre = keys_.size() > 1 ? value(keys_[1].c_str()) : 0.0f;
        auto c = r.withSizeKeepingCentre(std::min(r.getWidth(), r.getHeight()), std::min(r.getWidth(), r.getHeight()));
        g.setColour(dimLine);
        g.drawRect(c, 1.5f);
        g.drawEllipse(c.reduced(2), 1.5f);
        g.drawLine(c.getCentreX(), c.getY(), c.getCentreX(), c.getBottom(), 1.5f);
        g.drawLine(c.getX(), c.getCentreY(), c.getRight(), c.getCentreY(), 1.5f);
        const float s = 10.0f + dec * c.getWidth() * 0.35f, o = s * 0.35f;
        auto front = juce::Rectangle<float>(s, s).withCentre(c.getCentre().translated(-o / 2 + pre * 8.0f, o / 2));
        g.setColour(colour);
        g.drawRect(front, 1.5f);
        g.drawRect(front.translated(o, -o), 1.2f);
        for (auto p : {front.getTopLeft(), front.getTopRight(), front.getBottomLeft(), front.getBottomRight()})
            g.drawLine(p.x, p.y, p.x + o, p.y - o, 1.2f);
        return;
    }
    case Kind::Shelf: {
        const float f = value(keys_[0].c_str()), gain = value(keys_[1].c_str());
        const float x = r.getX() + r.getWidth() * (0.2f + 0.6f * f), yHi = r.getY() + r.getHeight() * 0.25f;
        const float yLo = r.getY() + r.getHeight() * (0.25f + 0.6f * (1.0f - gain));
        path.startNewSubPath(r.getX(), yHi);
        path.lineTo(x - 22, yHi);
        path.cubicTo(x - 8, yHi, x - 6, yLo, x + 8, yLo);
        path.lineTo(r.getRight(), yLo);
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.0f));
        g.setColour(colour);
        g.fillEllipse(x - 4.5f, yLo - 4.5f, 9, 9);
        return;
    }
    case Kind::ChorusWidth: {
        const float w = value(keys_[0].c_str());
        const float d = std::min(r.getHeight() * 0.7f, r.getWidth() * 0.4f), cx = r.getCentreX(), cy = r.getCentreY();
        const float off = d * 0.1f + w * d * 0.35f;
        g.setColour(colour.withMultipliedAlpha(0.9f));
        g.drawEllipse(cx - off - d / 2, cy - d / 2, d, d, 2.0f);
        g.drawEllipse(cx + off - d / 2, cy - d / 2, d, d, 2.0f);
        return;
    }
    case Kind::HighPass: {
        const float f = value(keys_[0].c_str());
        const float x = r.getX() + r.getWidth() * (0.08f + 0.6f * f), top = r.getY() + r.getHeight() * 0.3f;
        path.startNewSubPath(x - 18, r.getBottom());
        path.lineTo(x, top);
        path.lineTo(r.getRight(), top);
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.0f));
        g.setColour(colour);
        g.fillEllipse(x - 4.5f, top - 4.5f, 9, 9);
        return;
    }
    case Kind::SwarmLines: {
        // swarm: six detuned oscillators around the main one ("M"); spread by the detune, height by the mix
        const float det = value(keys_[0].c_str()), mix = keys_.size() > 1 ? value(keys_[1].c_str()) : 1.0f;
        const float base = r.getBottom(), cx = r.getCentreX(), top = r.getY() + 14;
        g.setColour(dimLine);
        g.drawLine(r.getX(), base, r.getRight(), base, 2.0f);
        const float step = 14.0f + det * 36.0f;
        for (int i = -3; i <= 3; ++i) {
            if (i == 0) continue;
            const float x = cx + (float)i * step;
            if (x < r.getX() || x > r.getRight()) continue;
            g.drawLine(x, base, x, base - (base - top) * (0.2f + 0.8f * mix), 2.0f);
        }
        g.setColour(colour);
        g.fillRect(cx - 4.0f, top, 8.0f, base - top);
        juce::Path m;
        m.addRectangle(cx - 7.0f, top - 12.0f, 14.0f, 10.0f);
        m.addTriangle(cx - 7.0f, top - 2.0f, cx + 7.0f, top - 2.0f, cx, top + 4.0f);
        g.fillPath(m);
        g.setColour(C(Tints::synth.group));
        g.setFont(uiFont(9.0f, true));
        g.drawText("M", juce::Rectangle<float>(cx - 7.0f, top - 12.0f, 14.0f, 10.0f), juce::Justification::centred);
        return;
    }
    case Kind::AnimIcon: {
        // swarm animation: zig-zag
        path.startNewSubPath(r.getX(), r.getY() + r.getHeight() * 0.1f);
        path.lineTo(r.getX() + r.getWidth() * 0.45f, r.getY() + r.getHeight() * 0.55f);
        path.lineTo(r.getX() + r.getWidth() * 0.55f, r.getY() + r.getHeight() * 0.4f);
        path.lineTo(r.getRight(), r.getBottom());
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.0f));
        return;
    }
    case Kind::NoiseDots: {
        const float v = keys_.empty() ? 0.5f : value(keys_[0].c_str());
        g.setColour(dimLine);
        uint32_t seed = 7;
        for (int i = 0; i < 22; ++i) {
            seed = seed * 1103515245u + 12345u;
            const float j = (float)((seed >> 16) & 0xff) / 255.0f - 0.5f;
            const float x = r.getX() + r.getWidth() * (float)i / 21.0f;
            const float y = r.getCentreY() + j * r.getHeight() * (0.2f + 0.8f * v) + std::sin((float)i * 0.6f) * r.getHeight() * 0.15f;
            g.fillEllipse(x - 1.8f, y - 1.8f, 3.6f, 3.6f);
        }
        return;
    }
    case Kind::SectionBar: {
        // FM Drum's section headings ("Body", "Noise & Transient", "Mix")
        g.setColour(C(0xff444e4d));
        g.fillRect(getLocalBounds());
        g.setColour(C(tint->title));
        g.setFont(uiFont(15.5f));
        g.drawText(keys_.empty() ? juce::String() : juce::String(keys_[0]), getLocalBounds(), juce::Justification::centred);
        return;
    }
    case Kind::OpEnvelope: {
        // modulation envelope of an operator: rise, decay to the end level; level bar on the right
        const float dec = value(keys_[0].c_str()), end = value(keys_[1].c_str()), lev = keys_.size() > 2 ? value(keys_[2].c_str()) : 0.0f;
        auto area = r.withTrimmedRight(30);
        const float top = area.getY() + 4, bottom = area.getBottom();
        const float endY = bottom - end * (bottom - top);
        // starts at the top and falls to the end level (straight down when the decay is 0); handles at
        // the start of the decay and on the end level
        const float x0 = area.getX() + 4, W = area.getWidth() - 4;
        path.startNewSubPath(x0, top);
        if (dec <= 0.001f) path.lineTo(x0, endY);
        else {
            const float tau = dec * W * 0.2f;
            for (float x = 0; x <= W; x += 1.5f) path.lineTo(x0 + x, endY + (top - endY) * std::exp(-x / tau));
        }
        path.lineTo(area.getRight(), endY);
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.2f));
        g.setColour(colour);
        const float xd = std::min(area.getRight() - 6, x0 + W * (0.33f + 0.6f * dec));
        for (auto p : {juce::Point<float>{x0, endY}, juce::Point<float>{xd, endY}}) g.fillEllipse(p.x - 4.5f, p.y - 4.5f, 9, 9);
        auto bar = juce::Rectangle<float>(r.getRight() - 12, r.getY(), 11, r.getHeight());
        g.setColour(colour.withMultipliedAlpha(0.18f));
        g.fillRect(bar);
        g.setColour(colour);
        g.fillRect(bar.withTop(bar.getBottom() - bar.getHeight() * lev));
        return;
    }
    case Kind::Ruler: {
        // tune "ruler": box with tick marks and the coarse value
        auto b = getLocalBounds().toFloat();
        g.setColour(C(0xff44504e));
        g.fillRect(b);
        g.setColour(C(0xff97b7b2).withAlpha(0.5f));
        for (int i = 0; i <= 16; ++i) {
            const float x = b.getX() + b.getWidth() * (float)i / 16.0f, h = i % 4 == 0 ? 6.0f : 3.0f;
            g.drawLine(x, b.getY() + 2, x, b.getY() + 2 + h, 1.0f);
            g.drawLine(x, b.getBottom() - 2, x, b.getBottom() - 2 - h, 1.0f);
        }
        g.drawLine(b.getCentreX(), b.getY(), b.getCentreX(), b.getBottom(), 1.0f);
        const int v = (raw(keys_[0].c_str()) - 0x4000) / 256;
        g.setColour(C(0xffa9b9b6));
        g.setFont(uiFont(19.0f));
        g.drawText(juce::String(v), b, juce::Justification::centred);
        return;
    }
    case Kind::PhaseStart: {
        const float v = value(keys_[0].c_str());
        const float x0 = r.getX() + 4, y0 = r.getBottom() - v * r.getHeight() * 0.8f;
        path.startNewSubPath(x0, y0);
        path.lineTo(r.getX() + r.getWidth() * 0.62f, r.getY());
        path.lineTo(r.getRight(), r.getY() + r.getHeight() * 0.42f);
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.0f));
        g.setColour(colour);
        g.fillEllipse(x0 - 4.5f, y0 - 4.5f, 9, 9);
        return;
    }
    case Kind::FeedbackIcon: {
        const auto c = r.getCentre();
        g.setColour(dimLine);
        for (float s : {13.0f, 7.0f}) {
            juce::Path p;
            p.addQuadrilateral(c.x - s, c.y, c.x, c.y - s, c.x + s, c.y, c.x, c.y + s);
            g.strokePath(p, juce::PathStrokeType(1.8f));
        }
        return;
    }
    case Kind::FoldIcon: {
        const float v = keys_.empty() ? 0.0f : value(keys_[0].c_str());
        path.startNewSubPath(r.getX(), r.getBottom());
        path.quadraticTo(r.getCentreX(), r.getY() - r.getHeight() * (0.6f + v), r.getRight(), r.getBottom());
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.0f));
        return;
    }
    case Kind::SimpleEnvelope: {
        // attack (optional) / hold / decay, as the noise and body envelopes of FM Drum and WaveTone
        const float atk = keys_.size() > 0 && !keys_[0].empty() ? value(keys_[0].c_str()) : 0.0f;
        // hold at its maximum is "Note" (held while the note is): not drawn, labelled "Note"
        const bool note = keys_.size() > 1 && raw(keys_[1].c_str()) >= 0x7F00;
        const float hold = keys_.size() > 1 && !note ? value(keys_[1].c_str()) : 0.0f;
        const float dec = keys_.size() > 2 ? value(keys_[2].c_str()) : 0.0f;
        const float seg = r.getWidth() / 3.2f, top = r.getY() + 4, bottom = r.getBottom();
        float x = r.getX() + 4;
        path.startNewSubPath(x, bottom);
        x += atk * seg;
        path.lineTo(x, top);
        x += hold * seg;
        path.lineTo(x, top);
        dots.push_back({x, top});
        if (note) {
            g.setColour(dimLine.withMultipliedAlpha(0.8f));
            g.setFont(uiFont(13.0f));
            g.drawText("Note", juce::Rectangle<float>(x + 14, top + 6, 60, 16), juce::Justification::centredLeft);
        }
        const bool inf = keys_.size() > 2 && raw(keys_[2].c_str()) >= 0x7F00; // INF: no decay
        if (inf) path.lineTo(r.getRight(), top);
        else {
            const float xd = std::min(r.getRight() - 6, x + 4 + dec * seg * 0.55f);
            path.cubicTo(x + 1, top + (bottom - top) * 0.6f, x + (xd - x) * 0.3f, bottom, xd, bottom);
            dots.push_back({xd, bottom});
            path.lineTo(r.getRight(), bottom);
        }
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.2f));
        g.setColour(colour);
        for (auto p : dots) g.fillEllipse(p.x - 4.5f, p.y - 4.5f, 9, 9);
        return;
    }
    case Kind::BandPass: {
        const float base = keys_.size() > 0 ? value(keys_[0].c_str()) : 0.0f, width = keys_.size() > 1 ? value(keys_[1].c_str()) : 0.0f;
        const float x0 = r.getX() + r.getWidth() * 0.12f + base * r.getWidth() * 0.4f;
        const float x1 = std::min(r.getRight() - 16, x0 + 16 + width * r.getWidth() * 0.45f);
        path.startNewSubPath(x0 - r.getWidth() * 0.1f, r.getBottom());
        path.lineTo(x0, r.getY() + 4);
        path.lineTo(x1, r.getY() + 4);
        path.lineTo(x1 + r.getWidth() * 0.1f, r.getBottom());
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.2f));
        g.setColour(colour);
        for (auto p : {juce::Point<float>{x0, r.getY() + 4}, juce::Point<float>{x1, r.getY() + 4}}) g.fillEllipse(p.x - 4.5f, p.y - 4.5f, 9, 9);
        return;
    }
    case Kind::Waveform: {
        // continuous waveform morph (sine -> triangle -> saw -> square) of the oscillator's wave value
        const float w = keys_.empty() ? 0.0f : value(keys_[0].c_str()) * 3.0f;
        const int k = juce::jlimit(0, 2, (int)w);
        const float f = w - (float)k;
        auto shape = [](int s, float ph) {
            switch (s) {
            case 0: return std::sin(ph * 6.2832f);
            case 1: return ph < 0.25f ? ph * 4 : ph < 0.75f ? 2 - ph * 4 : ph * 4 - 4;
            case 2: return 1.0f - 2.0f * ph;
            default: return ph < 0.5f ? 1.0f : -1.0f;
            }
        };
        const float periods = 1.0f; // one period
        for (int i = 0; i <= 160; ++i) {
            const float t = (float)i / 160.0f * periods, ph = t - std::floor(t);
            const float y = shape(k, ph) * (1.0f - f) + shape(k + 1, ph) * f;
            const float px = r.getX() + r.getWidth() * (float)i / 160.0f, py = r.getCentreY() - y * r.getHeight() * 0.48f;
            if (i == 0) path.startNewSubPath(px, py);
            else path.lineTo(px, py);
        }
        g.setColour(dimLine);
        g.strokePath(path, juce::PathStrokeType(2.0f));
        if (keys_.size() > 1) { // phase distortion position
            const float x = r.getX() + r.getWidth() * value(keys_[1].c_str());
            g.drawLine(x, r.getY() - 6, x, r.getBottom() + 6, 2.0f);
        }
        return;
    }
    case Kind::Decoration: {
        // drift, swarm, noise modulation: a static sketch
        const float a = keys_.empty() ? 0.5f : value(keys_[0].c_str());
        g.setColour(dimLine);
        for (int i = 0; i < 5; ++i) {
            juce::Path p;
            const float y = r.getY() + r.getHeight() * (0.15f + 0.175f * (float)i);
            p.startNewSubPath(r.getX(), y);
            for (int x = 0; x <= 40; ++x) {
                const float t = (float)x / 40.0f;
                p.lineTo(r.getX() + r.getWidth() * t, y + std::sin(t * 6.2832f * (1.0f + (float)i * 0.5f) + (float)i) * r.getHeight() * 0.06f * (0.3f + a));
            }
            g.strokePath(p, juce::PathStrokeType(1.5f));
        }
        return;
    }
    case Kind::DrumAlgorithm:
    case Kind::OscMod: {
        const bool drum = kind_ == Kind::DrumAlgorithm;
        const int v = juce::jlimit(0, drum ? 6 : 3, raw(drum ? "Sound_DrumAlgo" : "Sound_VaOscMod") >> 8);
        const DiagramColours dc{colour.withMultipliedAlpha(0.42f), colour.withMultipliedAlpha(0.85f)};
        const auto full = getLocalBounds().toFloat().reduced(2.0f); // the drawings keep their own margins
        if (drum) drawFmDrumAlgorithm(g, full, v, dc);
        else drawOscModulation(g, full, v, dc);
        return;
    }
    case Kind::FilterEnvelope:
    case Kind::AmpEnvelope:
        break;
    }
    // ADSR envelopes: handles at the start, the attack peak, the decay end,
    // the middle of the sustain and the release end; the release starts at a fixed place on the right.
    const bool amp = kind_ == Kind::AmpEnvelope;
    const float del = amp ? 0.0f : value("Sound_FilterEnvDelay");
    const float atk = value(amp ? "Sound_AmpAttack" : "Sound_FilterAttack");
    const float hold = amp && raw("Sound_AmpMode") > 0 ? value("Sound_AmpHold") : 0.0f;
    const float dec = value(amp ? "Sound_AmpDecay" : "Sound_FilterDecay");
    const float sus = value(amp ? "Sound_AmpSustain" : "Sound_FilterSustain");
    const float rel = value(amp ? "Sound_AmpRelease" : "Sound_FilterRelease");
    const float W = r.getWidth();
    const float top = r.getY() + 4, bottom = r.getBottom();
    const float susY = bottom - sus * (bottom - top);
    const float x0 = r.getX() + del * W * 0.2f;
    path.startNewSubPath(r.getX(), bottom);
    path.lineTo(x0, bottom);
    dots.push_back({x0, bottom});
    const float xa = x0 + atk * W * 0.2f;
    path.lineTo(xa, top);
    dots.push_back({xa, top});
    const float xh = xa + hold * W * 0.2f;
    if (hold > 0) path.lineTo(xh, top);
    const float xs = r.getRight() - W * 0.31f;
    const float xd = std::min(xs - 20.0f, xh + W * (0.012f + 0.21f * dec));
    path.cubicTo(xh + 1.0f, top + (susY - top) * 0.7f, xh + (xd - xh) * 0.3f, susY, xd, susY);
    dots.push_back({xd, susY});
    dots.push_back({(xd + xs) / 2, susY});
    path.lineTo(xs, susY);
    const float xr = xs + W * (0.01f + 0.25f * rel);
    path.cubicTo(xs + 1.0f, susY + (bottom - susY) * 0.7f, xs + (xr - xs) * 0.3f, bottom, xr, bottom);
    dots.push_back({xr, bottom});
    path.lineTo(r.getRight(), bottom);
    if (amp) {
        // the AHD / ADSR mode icons above the envelope
        const bool ahd = raw("Sound_AmpMode") > 0;
        auto icon = [&](float cx, bool adsr, bool on) {
            juce::Path p;
            const float y0 = r.getY() + 2, y1 = r.getY() + 14;
            if (adsr) {
                p.startNewSubPath(cx - 12, y1);
                p.lineTo(cx - 7, y0);
                p.lineTo(cx - 2, y0 + 6);
                p.lineTo(cx + 5, y0 + 6);
                p.lineTo(cx + 9, y1);
            } else {
                p.startNewSubPath(cx - 12, y1);
                p.lineTo(cx - 6, y0);
                p.lineTo(cx + 4, y0);
                p.lineTo(cx + 10, y1);
            }
            g.setColour(on ? colour : dimLine);
            g.strokePath(p, juce::PathStrokeType(1.8f));
        };
        icon(r.getCentreX() - 4, false, ahd);
        icon(r.getCentreX() + 30, true, !ahd);
    }
    g.setColour(dimLine);
    g.strokePath(path, juce::PathStrokeType(2.2f));
    g.setColour(colour);
    for (auto p : dots) g.fillEllipse(p.x - 4.5f, p.y - 4.5f, 9, 9);
}

// ------------------------------------------------------------------------------------------ LevelMeter
namespace {
float meterPos(float gain) // 0..1 on a -60..+3 dB scale
{
    if (gain <= 0.001f) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (20.0f * std::log10(gain) + 60.0f) / 63.0f);
}
} // namespace

void LevelMeter::timerCallback()
{
    bool changed = false;
    for (int c = 0; c < 2; ++c) {
        const float pk = meterPos(proc_.takeChannelPeak(channel_ + c));
        const float v = std::max(pk, level_[c] - 0.025f); // fall about 50 dB/s
        if (pk >= hold_[c]) hold_[c] = pk, holdFrames_[c] = 30;
        else if (--holdFrames_[c] <= 0) hold_[c] = std::max(0.0f, hold_[c] - 0.02f);
        changed = changed || std::abs(v - level_[c]) > 0.002f || holdFrames_[c] >= 0;
        level_[c] = v;
    }
    if (changed) repaint();
}

void LevelMeter::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float w = (r.getWidth() - 2.0f) / 2.0f;
    const float zero = 1.0f - meterPos(1.0f); // 0 dB line
    for (int c = 0; c < 2; ++c) {
        auto bar = juce::Rectangle<float>(r.getX() + c * (w + 2.0f), r.getY(), w, r.getHeight());
        g.setColour(C(0xff2a2a2a));
        g.fillRect(bar);
        const float top = bar.getBottom() - bar.getHeight() * level_[c];
        juce::ColourGradient grad(C(0xff7ccf9a), bar.getX(), bar.getBottom(), C(0xffe05a4f), bar.getX(), bar.getY(), false);
        grad.addColour(0.75, C(0xff7ccf9a));
        grad.addColour(0.9, C(0xffe6c35a));
        g.setGradientFill(grad);
        g.fillRect(bar.withTop(top));
        if (hold_[c] > 0.0f) {
            g.setColour(hold_[c] > 1.0f - zero ? C(0xffe05a4f) : C(0xffd8d8d8));
            g.fillRect(bar.getX(), bar.getBottom() - bar.getHeight() * hold_[c], bar.getWidth(), 2.0f);
        }
    }
    g.setColour(C(Palette::textDim));
    g.fillRect(r.getX(), r.getY() + r.getHeight() * zero, r.getWidth(), 1.0f);
}

// ------------------------------------------------------------------------------------------ Group
void Group::paint(juce::Graphics& g)
{
    g.setColour(C(tint_->group));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 2.0f);
    g.setFont(uiFont(15.5f));
    g.setColour(C(tint_->title));
    const int n = 1 + extraTitles.size();
    const int w = getWidth() / n;
    if (title_.isNotEmpty()) g.drawText(title_, 0, 8, w, 22, juce::Justification::centred);
    for (int i = 0; i < extraTitles.size(); ++i) g.drawText(extraTitles[i], (i + 1) * w, 8, w, 22, juce::Justification::centred);
}

// ------------------------------------------------------------------------------------------ Panel
void Panel::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(C(Palette::panel));
    g.fillRect(r);
    auto head = r.withHeight((float)kHeader + 2).reduced(2, 2);
    g.setColour(C(Palette::panelHeader));
    g.fillRect(head);
    g.setColour(C(Palette::panelBorder));
    g.drawRect(r.reduced(0.5f), 1.6f);
    g.setColour(C(Palette::panelTitle));
    g.setFont(uiFont(16.0f));
    g.drawText(title_, head.withTrimmedLeft(12), juce::Justification::centredLeft);
    if (rightText.isNotEmpty()) {
        g.setColour(C(Palette::text));
        g.setFont(uiFont(15.5f, true));
        g.drawText(rightText, head.withTrimmedRight(14), juce::Justification::centredRight);
    }
}

} // namespace obui
