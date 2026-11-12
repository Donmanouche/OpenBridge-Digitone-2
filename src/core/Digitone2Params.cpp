#include "Digitone2Params.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace ob {

const std::vector<ParamDef>& digitone2Params()
{
    static const std::vector<ParamDef> table = {
#include "Digitone2ParamTable.inc"
    };
    return table;
}

const ParamDef* findParam(const std::string& key)
{
    static const std::unordered_map<std::string, const ParamDef*> map = [] {
        std::unordered_map<std::string, const ParamDef*> m;
        for (auto& p : digitone2Params()) m[p.key] = &p;
        return m;
    }();
    auto it = map.find(key);
    return it == map.end() ? nullptr : it->second;
}

int machineOf(const ParamDef& p) { return p.object == ParamObject::Sound && p.section >= 0 && p.section <= 4 ? p.section : -1; }
int filterMachineOf(const ParamDef& p)
{
    return p.object == ParamObject::Sound && p.section >= 5 && p.section <= 10 ? p.section - 5 : -1;
}

const char* machineName(int m)
{
    static const char* n[] = {"FM Tone", "WaveTone", "FM Drum", "Swarmer", "MIDI"};
    return m >= 0 && m < 5 ? n[m] : "?";
}

const char* filterMachineName(int f)
{
    static const char* n[] = {"Multi-Mode", "Lowpass 4", "Equalizer", "Comb-", "Legacy LP/HP", "Comb+"};
    return f >= 0 && f < 6 ? n[f] : "?";
}

namespace {
// FM ratio lists.
const char* const kRatioA[36] = {"0.25", "0.50", "0.75", "1.00", "1.25", "1.50", "1.75", "2.00", "2.25",
                                 "2.50", "2.75", "3.00", "3.25", "3.50", "3.75", "4.00", "4.25", "4.50",
                                 "4.75", "5.00", "5.50", "6.00", "6.50", "7.00", "7.50", "8.00", "8.50",
                                 "9.00", "9.50", "10.0", "11.0", "12.0", "13.0", "14.0", "15.0", "16.0"};
const char* const kRatioC[19] = {"0.25", "0.50", "0.75", "1.00", "2.00", "3.00", "4.00", "5.00", "6.00", "7.00",
                                 "8.00", "9.00", "10.0", "11.0", "12.0", "13.0", "14.0", "15.0", "16.0"};

#include "Digitone2Formats.inc"

// signed value with two decimals from the fine byte
std::string signedDecimal(int v)
{
    if (v == 0) return "0.00";
    const int a = std::abs(v);
    const int frac = std::max((a & 0xff) * 100 >> 8, a < 8 ? 1 : 0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%d.%02d", v < 0 ? "-" : "", a >> 8, frac);
    return buf;
}
} // namespace

// signed value with one decimal
static std::string signedDecimal1(int v)
{
    if (v == 0) return "0.0";
    const int a = std::abs(v);
    const int frac = std::max((a & 0xff) * 10 >> 8, a < 8 ? 1 : 0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%d.%01d", v < 0 ? "-" : "", a >> 8, frac);
    return buf;
}

template <size_t N> static std::string pick(const char* const (&t)[N], int i)
{
    return i >= 0 && i < (int)N ? t[i] : "ERR";
}

std::string ratioBPart(int raw, bool b1)
{
    // (raw >> 1) = b1 * 19 + b2, shown "B2/B1"
    const int i = raw >> 1, hi = i / 19, lo = i % 19;
    if (hi < 0 || hi >= 19) return "ERR";
    return b1 ? kRatioC[hi] : kRatioC[lo];
}

std::string formatValue(const ParamDef& p, int raw)
{
    // One case per display format.
    char buf[40];
    const int c = raw >> 8;
    switch (p.format) {
    case ParamFormat::RatioA: return kRatioA[std::clamp(c, 0, 35)];
    case ParamFormat::RatioC: return kRatioC[std::clamp(c, 0, 18)];
    case ParamFormat::RatioB: return ratioBPart(raw, false) + "/" + ratioBPart(raw, true);
    case ParamFormat::Harmonics: return signedDecimal(raw - 0x3f00);
    case ParamFormat::RatioOffset: {
        const int t = (raw * 2000 >> 15) - 1000;
        if (t <= -1000) return "-1.000";
        if (t == 0) return "0.000";
        std::snprintf(buf, sizeof buf, "%s%03d", t < 0 ? "-0." : "+0.", std::abs(t));
        return buf;
    }
    case ParamFormat::OnOff: return (raw & ~0xff) ? "ON" : "OFF";
    case ParamFormat::PortaOnOff: return (raw & ~0xff) == 0x4000 ? "ON" : "OFF";
    case ParamFormat::Routing: return (raw & ~0xff) ? "Post Filter" : "Pre Filter";
    case ParamFormat::AmpMode: return (raw & ~0xff) ? "ADSR" : "AHD";
    case ParamFormat::Pan: {
        const int d = raw - 0x4000;
        if (d == 0) return "CEN";
        std::snprintf(buf, sizeof buf, "%s%d", d < 0 ? "L" : "R", std::abs(d) >> 8);
        return buf;
    }
    case ParamFormat::Bipolar: std::snprintf(buf, sizeof buf, "%d", (raw - 0x4000) >> 8); return buf;
    case ParamFormat::BipolarDecimal: return signedDecimal(raw - 0x4000);
    case ParamFormat::LfoDepth: return signedDecimal(raw * 2 - 0x8000);
    case ParamFormat::Decimal: return signedDecimal(raw);
    case ParamFormat::Decimal1: return signedDecimal1(raw);
    case ParamFormat::DecimalInf: return (raw & ~0xff) == 0x7f00 ? "INF" : signedDecimal(raw);
    case ParamFormat::DecayInf:
        if (c == 127) return "INF";
        break;
    case ParamFormat::HoldNote:
        if (c == 127) return "NOTE";
        break;
    case ParamFormat::SendOff:
        if (raw == 0) return "OFF";
        break;
    case ParamFormat::Percent: std::snprintf(buf, sizeof buf, "%d%%", c); return buf;
    case ParamFormat::PlusOne: std::snprintf(buf, sizeof buf, "%d", (raw + 0x100) >> 8); return buf;
    case ParamFormat::MinusTwo: std::snprintf(buf, sizeof buf, "%d", c - 2); return buf;
    case ParamFormat::LfoMult: {
        // 2^(n mod 12); prefixed "x" (synced to the tempo, n < 12) or a
        // dot (free running, n >= 12; HIDDEN TEARS' LFO1 = 19 is shown as "• 128")
        const int r = c % 12, v = 1 << r;
        const char* prefix = c >= 12 ? "\xe2\x80\xa2 " : "\xc3\x97 "; // "• " or "× "
        if (r >= 10) std::snprintf(buf, sizeof buf, "%s%dk", prefix, v >> 10);
        else std::snprintf(buf, sizeof buf, "%s%d", prefix, v);
        return buf;
    }
    case ParamFormat::LfoWave: {
        static const char* const w[] = {"TRI", "SINE", "SQR", "SAW", "EXPO", "RAMP", "RAND"};
        return pick(w, c);
    }
    case ParamFormat::LfoTrig: {
        static const char* const t[] = {"FREE", "TRIG", "HOLD", "ONE", "HALF"};
        return pick(t, c);
    }
    case ParamFormat::PhaseReset: {
        static const char* const t[] = {"OFF", "ALL", "C", "A+B1+B2", "A+B2"};
        return pick(t, c);
    }
    case ParamFormat::OscMod: {
        static const char* const t[] = {"Off", "Ring Mod", "Ring Mod Fixed", "Hard Sync"};
        return pick(t, c);
    }
    case ParamFormat::OscPhaseReset: {
        static const char* const t[] = {"Off", "On", "Random"};
        return pick(t, c);
    }
    case ParamFormat::WaveTable: {
        static const char* const t[] = {"Prim.", "Harm."};
        return pick(t, c);
    }
    case ParamFormat::LegacyType: {
        static const char* const t[] = {"LP", "HP", "OFF"};
        return t[std::clamp(c, 0, 2)];
    }
    case ParamFormat::LinearOffset: std::snprintf(buf, sizeof buf, "%.2f", (raw - 0x3200) / 12800.0f * 10.0f); return buf;
    case ParamFormat::DrumRatio: {
        // thousandths, 3 decimals below 10, 2 above
        const int n = (raw * 0x7cff >> 15) + 1, i = n / 1000, f = n % 1000;
        if (f == 0) std::snprintf(buf, sizeof buf, "%d.0", i);
        else if (i < 10) std::snprintf(buf, sizeof buf, "%d.%03d", i, f);
        else std::snprintf(buf, sizeof buf, "%d.%02d", i, f / 10);
        return buf;
    }
    case ParamFormat::BitReduction: return pick(kBitReduction, c);
    case ParamFormat::MakeupGain: return pick(kMakeupGain, c) + "dB";
    case ParamFormat::CompRatio: return pick(kCompRatio, c);
    case ParamFormat::SidechainSource: return pick(kSidechainSource, c);
    case ParamFormat::DelayFeedback: std::snprintf(buf, sizeof buf, "%d", c * 100 / 64); return buf;
    case ParamFormat::ReverbDecay:
        if (c > 126) return "INF";
        std::snprintf(buf, sizeof buf, "%d", std::max(c, 0) + 1);
        return buf;
    case ParamFormat::Plain:
    default: break;
    }
    std::snprintf(buf, sizeof buf, "%d", c);
    return buf;
}

int rawFromNormalised(const ParamDef& p, float norm)
{
    norm = std::clamp(norm, 0.0f, 1.0f);
    int raw = p.min + (int)std::lround(norm * (float)(p.max - p.min));
    if (!p.decimals && p.format != ParamFormat::Decimal && p.format != ParamFormat::BipolarDecimal &&
        p.format != ParamFormat::RatioB && p.format != ParamFormat::RatioOffset && p.format != ParamFormat::Harmonics &&
        p.format != ParamFormat::LfoDepth && p.format != ParamFormat::Decimal1 && p.format != ParamFormat::DecimalInf &&
        p.format != ParamFormat::LinearOffset && p.format != ParamFormat::DrumRatio)
        raw = (raw + 128) & ~0xff;
    return std::clamp(raw, p.min, p.max);
}

float normalisedFromRaw(const ParamDef& p, int raw)
{
    if (p.max <= p.min) return 0.0f;
    return std::clamp((float)(raw - p.min) / (float)(p.max - p.min), 0.0f, 1.0f);
}

} // namespace ob
