// Digitone II parameter descriptions (generated table) and helpers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ob {

enum class ParamObject : uint8_t { Sound, Fx, Kit, Settings };
// Display formats of the parameter values.
enum class ParamFormat : uint8_t {
    Plain, Bipolar, Decimal, BipolarDecimal, OnOff, Pan, RatioA, RatioC, RatioB, Harmonics, RatioOffset,
    LfoDepth, DecayInf, SendOff, Decimal1, Percent, Routing, LfoMult, LfoWave, LfoTrig, PlusOne, LinearOffset,
    WaveTable, DrumRatio, DecimalInf, PortaOnOff, LegacyType, HoldNote, AmpMode, BitReduction, PhaseReset, OscMod,
    OscPhaseReset, MinusTwo, DelayFeedback, ReverbDecay, MakeupGain, CompRatio, SidechainSource
};

struct ParamDef {
    const char* key;      // Overbridge identifier, e.g. "Sound_AmpVolume"
    ParamObject object;
    int metaIndex;        // index in the parameter description table
    int pid;              // id the device uses in messages
    int section;          // 0..4 machines, 5..10 filter machines, 11 amp, 13 filter, 15 fx sends, 26..28 LFO ...
    int knob;             // position on the device page
    int min, max, def;    // raw u16 values (coarse << 8 | fine)
    bool decimals;        // shows the fine part
    int cc, nrpn;         // MIDI CC / NRPN (MSB << 7 | LSB), -1 if none
    ParamFormat format;
    int slot;             // u16 index in the sound container, -1 unknown / not stored there
    const char* name;
    const char* sectionName;
    const char* label;    // device screen label
};

const std::vector<ParamDef>& digitone2Params();
const ParamDef* findParam(const std::string& key);

// Section of the machine / filter machine a Sound parameter belongs to, or -1 if it is common.
int machineOf(const ParamDef& p);        // 0..4
int filterMachineOf(const ParamDef& p);  // 0..5

const char* machineName(int machine);       // "FM Tone" ...
const char* filterMachineName(int filter);  // "Multi-Mode" ...

// Value text as on the device (coarse value, optional fine decimals, bipolar centred on 64...).
// Ratio B is a pair: B1 (b1 = true) or B2 of the raw value.
std::string ratioBPart(int raw, bool b1);
std::string formatValue(const ParamDef& p, int raw);
// Normalised 0..1 <-> raw, quantised to coarse steps unless the parameter has decimals.
int rawFromNormalised(const ParamDef& p, float norm);
float normalisedFromRaw(const ParamDef& p, int raw);

constexpr int kSoundSlots = 106;
constexpr int kSoundSlotOffset = 28;      // first u16 in the sound container
constexpr int kSoundMachineOffset = 0xF4; // u8 machine type
constexpr int kSoundFilterOffset = 0xF5;  // u8 filter machine type
constexpr int kSoundNameOffset = 12;      // 16 chars

} // namespace ob
