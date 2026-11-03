// tracker-mini-ce: the WAVE and PHASE synth engines for CE's open synth SDK.
//
// Ported from Modular-Trackular's synths patch (MIT, Copyright (c) 2026 Modular-Trackular
// contributors). There, both took the place of a stock model and borrowed its parameter
// list; here they are engines of their own. The DSP (core/libs/ceSynths/) is the
// original C, unchanged: each voice keeps a float array with the parameters in the
// engines' own order and units (wt_engine.h's WP_*, ph_engine.h's PP_*), so parameter
// ids 0-60 are those indices, and 61-64 the per-synth chorus.
//
// WAVE: a wavetable synth after the PPG Wave and Ensoniq ESQ-1 (two oscillators with
//   7 morphing wavetable banks, a super saw and a saw; Digital mode; ladder, OB and SVF
//   filters; three envelopes, two LFOs and a 6-slot mod matrix).
// PHASE: a phase distortion synth after the Casio CZ series (two DCOs with 8 shapes,
//   alternating pairs, ring and noise mod; DCW, DCA and DCO envelopes; LFOs; matrix).

#include "CeSynthVoices.h"
#include "config.h"

#ifdef FEATURE_SYNTH

#include "PolySynth/sdk/SynthVoiceSDK.h"
#include "ceSynths/ceSynths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace polysynth
{
namespace
{

enum class CeParamId : int
{
    kFirst = 0,
    kChorus = 61,
    kChorusRate = 62,
    kChorusDepth = 63,
    kChorusMix = 64,
    kLast = 64,
};

constexpr int kParamCount = 65;
static_assert(enumutil::EnumMax<CeParamId>::value + 1 == kParamCount, "parameter count");
static_assert(CESYN_ENGINE_PARAMS == static_cast<int>(CeParamId::kChorus), "engine parameters");

using Curve = Patch::Metadata::Param::Curve;
using MacroOp = Patch::Metadata::Param::MacroOperation;

enum class Fmt : uint8_t
{
    kPlain,
    kPercent,       // 0..1 -> 0..100 %
    kSignedPercent, // -x..x -> signed %
    kTime,
    kHz,
    kSemis,
    kCents,
    kList,
    kLfoRate,    // Hz, or a tempo division when the LFO's Sync (aux) is on
    kChorusRate, // 0..1 -> 0.05..5 Hz
};

struct Def
{
    const char* name;
    float def, lo, hi;
    Fmt fmt;
    const char* const* list = nullptr;
    uint8_t aux = 0; // list: item count; LFO rate: the Sync parameter
    Curve curve = Curve::kLinear;
    MacroOp op = MacroOp::kAdd;
};

const char* const kOffOn[] = { "Off", "On" };
const char* const kGlideModes[] = { "Always", "Overlap", "Legato", "Legato O." };
const char* const kLfoWaves[] = { "Triangle", "Sine", "Ramp Up", "Ramp Down", "Square", "Random S&H" };
const char* const kLfoRetrig[] = { "Note", "One Shot", "Free" };
const char* const kPolarity[] = { "Bipolar", "Unipolar" };
const char* const kSyncDivs[] = { "1",    "1/2 D", "1/2",  "1/4 D",  "1 T",   "1/4",    "1/8 D", "1/2 T",
                                  "1/8",  "1/16 D", "1/4 T", "1/16", "1/8 T", "1/32", "1/16 T", "1/32 T" };

// WAVE
const char* const kWaves[] = { "Formant", "Resonant", "Harmonic", "Square/PWM", "Sin/Tri/Saw/Sqr",
                               "Sync",    "FM",       "Super Saw", "Saw" };
const char* const kFilterTypes[] = { "LP Ladder 24", "LP OB 24",  "LP OB 12",  "LP SVF 24", "LP SVF 12",
                                     "HP OB 24",     "HP OB 12",  "HP SVF 24", "HP SVF 12", "BP OB 24",
                                     "BP OB 12",     "BP SVF 24", "BP SVF 12", "Notch 24",  "Notch 12" };
const char* const kWaveSrcs[] = { "Amp Env", "Filter Env", "Mod Env", "LFO 1", "LFO 2", "Velocity", "Key" };
const char* const kWaveDests[] = { "Position", "Position 1", "Position 2", "Pitch", "Pitch 1", "Pitch 2",
                                   "Mix",      "Cutoff",     "Resonance",  "Volume", "Noise" };
// PHASE
const char* const kShapes[] = { "Saw", "Square", "Pulse", "Double Sine", "Saw-Pulse", "Reso I", "Reso II", "Reso III" };
const char* const kPairs[] = { "Off",       "Saw",    "Square",  "Pulse",   "Double Sine",
                               "Saw-Pulse", "Reso I", "Reso II", "Reso III" };
const char* const kPhaseSrcs[] = { "DCA Env", "DCW Env", "DCO Env", "LFO 1", "LFO 2", "Velocity", "Key" };
const char* const kPhaseDests[] = { "Pitch", "Pitch 1", "Pitch 2", "DCW", "DCW 1", "DCW 2", "Mix", "Volume", "Detune" };

#define LIST(names) Fmt::kList, names, static_cast<uint8_t>(std::size(names))

constexpr Def adsr(const char* name, float def, int part)
{
    return part == 2 ? Def{ name, def, 0.0f, 1.0f, Fmt::kPercent }
                     : Def{ name, def, 0.0f, 10.0f, Fmt::kTime, nullptr, 0, Curve::kPower };
}

constexpr Def lfoRate(const char* name, float def, int syncId)
{
    return Def{ name, def, 0.0f, 100.0f, Fmt::kLfoRate, nullptr, static_cast<uint8_t>(syncId), Curve::kPower };
}

constexpr Def kChorusDefs[4] = {
    { "Chorus", 0.0f, 0.0f, 1.0f, LIST(kOffOn) },
    { "Chorus Rate", 0.5f, 0.0f, 1.0f, Fmt::kChorusRate },
    { "Chorus Depth", 0.5f, 0.0f, 1.0f, Fmt::kPercent },
    { "Chorus Mix", 0.5f, 0.0f, 1.0f, Fmt::kPercent },
};

std::array<Def, kParamCount> makeWaveDefs()
{
    std::array<Def, kParamCount> d{};
    d[0] = { "Position 1", 0.6667f, 0.0f, 1.0f, Fmt::kPercent };
    d[1] = { "Position 2", 0.6667f, 0.0f, 1.0f, Fmt::kPercent };
    d[2] = { "Wave 1", 4.0f, 0.0f, 8.0f, LIST(kWaves) };
    d[3] = { "Wave 2", 4.0f, 0.0f, 8.0f, LIST(kWaves) };
    d[4] = { "Mix", 0.0f, -1.0f, 1.0f, Fmt::kSignedPercent };
    d[5] = { "Tune 1", 0.0f, -36.0f, 36.0f, Fmt::kSemis };
    d[6] = { "Tune 2", 0.0f, -36.0f, 36.0f, Fmt::kSemis };
    d[7] = { "Detune", 8.0f, 0.0f, 100.0f, Fmt::kCents };
    d[8] = { "Finetune", 0.0f, -100.0f, 100.0f, Fmt::kCents };
    d[9] = { "Reset 1", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[10] = { "Reset 2", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[11] = { "Type", 0.0f, 0.0f, 14.0f, LIST(kFilterTypes) };
    d[12] = { "Cutoff", 2400.0f, 20.0f, 20000.0f, Fmt::kHz, nullptr, 0, Curve::kExponential, MacroOp::kMult };
    d[13] = { "Resonance", 0.2f, 0.0f, 1.5f, Fmt::kPercent };
    d[14] = { "Env Amt", 0.15f, -1.0f, 1.0f, Fmt::kSignedPercent };
    d[15] = adsr("Flt Attack", 0.005f, 0);
    d[16] = adsr("Flt Decay", 0.8f, 1);
    d[17] = adsr("Flt Sustain", 0.35f, 2);
    d[18] = adsr("Flt Release", 0.5f, 3);
    d[19] = adsr("Attack", 0.005f, 0);
    d[20] = adsr("Decay", 0.5f, 1);
    d[21] = adsr("Sustain", 0.8f, 2);
    d[22] = adsr("Release", 0.4f, 3);
    d[23] = { "Glide Mode", 0.0f, 0.0f, 3.0f, LIST(kGlideModes) };
    d[24] = { "Glide Time", 0.0f, 0.0f, 10.0f, Fmt::kTime, nullptr, 0, Curve::kPower };
    d[25] = { "Key Track", 0.5f, -2.0f, 2.0f, Fmt::kSignedPercent };
    d[26] = { "Digital", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[27] = { "Noise", 0.0f, 0.0f, 1.0f, Fmt::kPercent };
    d[28] = adsr("Mod Attack", 0.5f, 0);
    d[29] = adsr("Mod Decay", 1.0f, 1);
    d[30] = adsr("Mod Sustain", 0.0f, 2);
    d[31] = adsr("Mod Release", 0.5f, 3);
    d[32] = lfoRate("LFO 1 Rate", 1.0f, 35);
    d[33] = { "LFO 1 Wave", 0.0f, 0.0f, 5.0f, LIST(kLfoWaves) };
    d[34] = { "LFO 1 Retrig", 2.0f, 0.0f, 2.0f, LIST(kLfoRetrig) };
    d[35] = { "LFO 1 Sync", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[36] = lfoRate("LFO 2 Rate", 0.25f, 39);
    d[37] = { "LFO 2 Wave", 1.0f, 0.0f, 5.0f, LIST(kLfoWaves) };
    d[38] = { "LFO 2 Retrig", 2.0f, 0.0f, 2.0f, LIST(kLfoRetrig) };
    d[39] = { "LFO 2 Sync", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    static const char* const srcNames[6] = { "Src 1", "Src 2", "Src 3", "Src 4", "Src 5", "Src 6" };
    static const char* const dstNames[6] = { "Dest 1", "Dest 2", "Dest 3", "Dest 4", "Dest 5", "Dest 6" };
    static const char* const amtNames[6] = { "Amt 1", "Amt 2", "Amt 3", "Amt 4", "Amt 5", "Amt 6" };
    for (int s = 0; s < 6; s++)
    {
        float src = s == 0 ? 3.0f : (s == 1 ? 2.0f : 0.0f); // LFO 1, Mod Env: routings to try
        d[40 + 3 * s] = { srcNames[s], src, 0.0f, 6.0f, LIST(kWaveSrcs) };
        d[41 + 3 * s] = { dstNames[s], 0.0f, 0.0f, 10.0f, LIST(kWaveDests) };
        d[42 + 3 * s] = { amtNames[s], 0.0f, -1.0f, 1.0f, Fmt::kSignedPercent };
    }
    d[58] = { "Drive", 0.0f, 0.0f, 1.0f, Fmt::kPercent };
    d[59] = { "LFO 1 Polarity", 0.0f, 0.0f, 1.0f, LIST(kPolarity) };
    d[60] = { "LFO 2 Polarity", 0.0f, 0.0f, 1.0f, LIST(kPolarity) };
    for (int k = 0; k < 4; k++)
        d[61 + k] = kChorusDefs[k];
    return d;
}

std::array<Def, kParamCount> makePhaseDefs()
{
    std::array<Def, kParamCount> d{};
    d[0] = { "Wave 1+", 0.0f, 0.0f, 8.0f, LIST(kPairs) };
    d[1] = { "Wave 2+", 0.0f, 0.0f, 8.0f, LIST(kPairs) };
    d[2] = { "Wave 1", 0.0f, 0.0f, 7.0f, LIST(kShapes) };
    d[3] = { "Wave 2", 0.0f, 0.0f, 7.0f, LIST(kShapes) };
    d[4] = { "Mix", 0.0f, -1.0f, 1.0f, Fmt::kSignedPercent };
    d[5] = { "Tune 1", 0.0f, -36.0f, 36.0f, Fmt::kSemis };
    d[6] = { "Tune 2", 0.0f, -36.0f, 36.0f, Fmt::kSemis };
    d[7] = { "Detune", 8.0f, 0.0f, 100.0f, Fmt::kCents };
    d[8] = { "Finetune", 0.0f, -100.0f, 100.0f, Fmt::kCents };
    d[9] = { "Reset 1", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[10] = { "Reset 2", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[11] = { "Ring Mod", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[12] = { "DCW 1", 0.5f, 0.0f, 1.0f, Fmt::kPercent };
    d[13] = { "DCW 2", 0.5f, 0.0f, 1.0f, Fmt::kPercent };
    d[14] = { "Env Amt 1", 0.3f, -1.0f, 1.0f, Fmt::kSignedPercent };
    d[15] = adsr("DCO Attack", 0.005f, 0);
    d[16] = adsr("DCO Decay", 0.3f, 1);
    d[17] = adsr("DCO Sustain", 0.0f, 2);
    d[18] = adsr("DCO Release", 0.3f, 3);
    d[19] = adsr("Attack", 0.005f, 0);
    d[20] = adsr("Decay", 0.5f, 1);
    d[21] = adsr("Sustain", 0.8f, 2);
    d[22] = adsr("Release", 0.4f, 3);
    d[23] = adsr("DCW Attack", 0.005f, 0);
    d[24] = adsr("DCW Decay", 0.6f, 1);
    d[25] = adsr("DCW Sustain", 0.3f, 2);
    d[26] = adsr("DCW Release", 0.5f, 3);
    d[27] = lfoRate("LFO 1 Rate", 1.0f, 30);
    d[28] = { "LFO 1 Wave", 0.0f, 0.0f, 5.0f, LIST(kLfoWaves) };
    d[29] = { "LFO 1 Retrig", 2.0f, 0.0f, 2.0f, LIST(kLfoRetrig) };
    d[30] = { "LFO 1 Sync", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[31] = lfoRate("LFO 2 Rate", 0.25f, 34);
    d[32] = { "LFO 2 Wave", 1.0f, 0.0f, 5.0f, LIST(kLfoWaves) };
    d[33] = { "LFO 2 Retrig", 2.0f, 0.0f, 2.0f, LIST(kLfoRetrig) };
    d[34] = { "LFO 2 Sync", 0.0f, 0.0f, 1.0f, LIST(kOffOn) };
    d[35] = { "Glide Mode", 0.0f, 0.0f, 3.0f, LIST(kGlideModes) };
    d[36] = { "Glide Time", 0.0f, 0.0f, 10.0f, Fmt::kTime, nullptr, 0, Curve::kPower };
    d[37] = { "Key Follow", 0.0f, -1.0f, 1.0f, Fmt::kSignedPercent };
    d[38] = { "Env Amt 2", 0.3f, -1.0f, 1.0f, Fmt::kSignedPercent };
    d[39] = { "Noise Mod", 0.0f, 0.0f, 1.0f, Fmt::kPercent };
    d[40] = { "DCO Env Amt", 0.0f, -48.0f, 48.0f, Fmt::kSemis };
    static const char* const srcNames[6] = { "Src 1", "Src 2", "Src 3", "Src 4", "Src 5", "Src 6" };
    static const char* const dstNames[6] = { "Dest 1", "Dest 2", "Dest 3", "Dest 4", "Dest 5", "Dest 6" };
    static const char* const amtNames[6] = { "Amt 1", "Amt 2", "Amt 3", "Amt 4", "Amt 5", "Amt 6" };
    for (int s = 0; s < 6; s++)
    {
        float src = s == 0 ? 3.0f : (s == 1 ? 4.0f : 0.0f); // LFO 1 -> Pitch, LFO 2 -> DCW
        float dst = s == 1 ? 3.0f : 0.0f;
        d[41 + 3 * s] = { srcNames[s], src, 0.0f, 6.0f, LIST(kPhaseSrcs) };
        d[42 + 3 * s] = { dstNames[s], dst, 0.0f, 8.0f, LIST(kPhaseDests) };
        d[43 + 3 * s] = { amtNames[s], 0.0f, -1.0f, 1.0f, Fmt::kSignedPercent };
    }
    d[59] = { "LFO 1 Polarity", 0.0f, 0.0f, 1.0f, LIST(kPolarity) };
    d[60] = { "LFO 2 Polarity", 0.0f, 0.0f, 1.0f, LIST(kPolarity) };
    for (int k = 0; k < 4; k++)
        d[61 + k] = kChorusDefs[k];
    return d;
}

#undef LIST

std::function<std::string(const Patch&, Patch::paramvalue_t)> makeFormatter(const Def& d)
{
    switch (d.fmt)
    {
        case Fmt::kPercent: return sdk::createPercentFormatter();
        case Fmt::kSignedPercent:
            return [](const Patch&, Patch::paramvalue_t v)
            {
                int pc = static_cast<int>(std::lround(v * 100.0f));
                return pc > 0 ? fmt::format("+{:d} %", pc) : fmt::format("{:d} %", pc);
            };
        case Fmt::kTime: return sdk::createTimeFormatter();
        case Fmt::kHz: return sdk::createFrequencyFormatter();
        case Fmt::kSemis: return sdk::createSemitoneFormatter();
        case Fmt::kCents: return sdk::createCentsFormatter();
        case Fmt::kList:
        {
            const char* const* list = d.list;
            int count = d.aux;
            return [list, count](const Patch&, Patch::paramvalue_t v)
            { return std::string(list[std::clamp<int>(std::lround(v), 0, count - 1)]); };
        }
        case Fmt::kLfoRate:
        {
            int syncId = d.aux;
            return [syncId](const Patch& patch, Patch::paramvalue_t v)
            {
                if (patch.getParamValue(syncId).value_or(0.0f) > 0.5f)
                {
                    return std::string(kSyncDivs[std::clamp<int>(std::lround(v), 0, 15)]);
                }
                return v < 10.0f ? fmt::format("{:.2f} Hz", v) : fmt::format("{:.1f} Hz", v);
            };
        }
        case Fmt::kChorusRate:
            return [](const Patch&, Patch::paramvalue_t v)
            {
                float hz = 0.05f * std::pow(100.0f, std::clamp(v, 0.0f, 1.0f));
                return fmt::format("{:.2f} Hz", hz);
            };
        case Fmt::kPlain:
        default:
            return [](const Patch&, Patch::paramvalue_t v) { return fmt::format("{:.2f}", v); };
    }
}

template<int Engine>
class CeSynthVoice : public sdk::CustomSynthVoice<CeSynthVoice<Engine>, CeParamId>
{
    using Base = sdk::CustomSynthVoice<CeSynthVoice<Engine>, CeParamId>;

  public:
    explicit CeSynthVoice(int voiceIdx)
    {
        engine.reset(new uint8_t[cesyn_voice_size()]);
        cesyn_voice_init(engine.get(), Engine);
        const auto& defs = Engine == CESYN_PHASE ? phaseDefs() : waveDefs();
        for (int k = 0; k < kParamCount; k++)
        {
            p[k] = defs[k].def;
        }
        if (voiceIdx == 0) // the chorus runs on the mixdown, on voice 0
        {
            fx.reset(new uint8_t[cesyn_fx_size()]);
            cesyn_fx_init(fx.get());
        }
    }

    bool isActive() final { return cesyn_sounding(engine.get(), Engine) != 0; }

    void onNoteOn(const InstrumentVoice::InstrumentVoiceContext&,
                  note_t note,
                  float velocity,
                  bool /*isLegatoStep: the engine's Glide Mode decides*/) final
    {
        cesyn_note_on(engine.get(), Engine, p, note, velocity);
    }

    void onNoteOff(const InstrumentVoice::InstrumentVoiceContext&, note_t) final
    {
        cesyn_note_off(engine.get(), Engine);
    }

    void renderVoiceAudio(const InstrumentVoice::InstrumentVoiceContext& ctx,
                          std::optional<AudioBufferStereoRef>,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef>) final
    {
        auto left = outputBuffer[0];
        auto right = outputBuffer[1];
        cesyn_set_bend(engine.get(), Engine, this->pitchBendSemitones);
        if (left.size() == kBlock)
        {
            cesyn_render(engine.get(), Engine, p, left.data(), ctx.tempo);
        }
        else
        {
            std::fill(left.begin(), left.end(), 0.0f);
        }
        std::copy(left.begin(), left.end(), right.begin());
    }

    void renderPostProcessAudio(const InstrumentVoice::InstrumentVoiceContext&,
                                AudioBufferStereoRef outputBuffer) final
    {
        if (!fx || outputBuffer[0].size() != kBlock)
        {
            return;
        }
        float params[4] = { p[61], p[62], p[63], p[64] };
        cesyn_fx_run(fx.get(), params, outputBuffer[0].data(), outputBuffer[1].data());
    }

    static void createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
    {
        using EditorSection = Patch::Metadata::EditorLayout::EditorSection;
        using EditorElement = Patch::Metadata::EditorLayout::EditorElement;

        metadata.params.resize(kParamCount);
        metadata.params.shrink_to_fit();
        if (Base::paramHandlers.empty())
        {
            Base::paramHandlers.resize(kParamCount);
        }
        sdk::ParamHelper<CeSynthVoice> helper(metadata, Base::paramHandlers);
        addAll(helper, std::make_index_sequence<kParamCount>{});

        auto elems = [](std::initializer_list<int> ids)
        {
            std::vector<EditorElement> v;
            for (int id : ids)
            {
                v.push_back(EditorElement{ .type = EditorElement::Type::kParam, .id = id });
            }
            return v;
        };

        if (Engine == CESYN_PHASE)
        {
            metadata.layoutFlat.sections = {
                EditorSection{ .name = "Oscillator", .shortName = "DCO",
                               .elements = elems({ 2, 0, 3, 1, 4, 5, 6, 7, 8, 9, 10, 11, 39 }) },
                EditorSection{ .name = "DCW Filter", .shortName = "DCW",
                               .elements = elems({ 12, 13, 14, 38, 37, 23, 24, 25, 26 }) },
                EditorSection{ .name = "Amp Envelope", .shortName = "DCA",
                               .elements = elems({ 19, 20, 21, 22 }) },
                EditorSection{ .name = "Modulation", .shortName = "Mod",
                               .elements = elems({ 15, 16, 17, 18, 40, 27, 28, 29, 30, 59, 31, 32, 33, 34, 60 }) },
                EditorSection{ .name = "Modulation Matrix", .shortName = "Matrix",
                               .elements = elems({ 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58 }) },
                EditorSection{ .name = "Voice", .shortName = "Voice",
                               .elements = elems({ 35, 36, 61, 62, 63, 64 }) },
            };
        }
        else
        {
            metadata.layoutFlat.sections = {
                EditorSection{ .name = "Oscillator", .shortName = "Osc",
                               .elements = elems({ 2, 0, 5, 9, 3, 1, 6, 10, 4, 7, 8, 26, 27 }) },
                EditorSection{ .name = "Filter", .shortName = "Filter",
                               .elements = elems({ 11, 12, 13, 14, 25, 58, 15, 16, 17, 18 }) },
                EditorSection{ .name = "Amp Envelope", .shortName = "Amp",
                               .elements = elems({ 19, 20, 21, 22 }) },
                EditorSection{ .name = "Modulation", .shortName = "Mod",
                               .elements = elems({ 28, 29, 30, 31, 32, 33, 34, 35, 59, 36, 37, 38, 39, 60 }) },
                EditorSection{ .name = "Modulation Matrix", .shortName = "Matrix",
                               .elements = elems({ 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57 }) },
                EditorSection{ .name = "Voice", .shortName = "Voice",
                               .elements = elems({ 23, 24, 61, 62, 63, 64 }) },
            };
        }
        metadata.layout23 = metadata.layoutFlat;

        if (Engine == CESYN_PHASE)
        {
            metadata.initDefaultMacros = [](std::vector<Patch::Metadata::Macro>& macros)
            {
                macros[0] = { .displayName = "Attack", .modifiers = { { 19, 0.5f } } };
                macros[1] = { .displayName = "Decay/Release",
                              .modifiers = { { 20, 0.5f }, { 24, 0.5f }, { 22, 0.5f }, { 26, 0.5f } } };
                macros[2] = { .displayName = "DCW", .modifiers = { { 12, 0.5f }, { 13, 0.5f } } };
                macros[3] = { .displayName = "DCW Env Amt", .modifiers = { { 14, 0.5f }, { 38, 0.5f } } };
                macros[4] = { .displayName = "Detune", .modifiers = { { 7, 0.5f } } };
                macros[5] = { .displayName = "Mix", .modifiers = { { 4, 0.5f } }, .bipolar = true };
            };
            metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kPhaseSynth);
        }
        else
        {
            metadata.initDefaultMacros = [](std::vector<Patch::Metadata::Macro>& macros)
            {
                macros[0] = { .displayName = "Attack", .modifiers = { { 19, 0.5f } } };
                macros[1] = { .displayName = "Decay/Release",
                              .modifiers = { { 20, 0.5f }, { 16, 0.5f }, { 22, 0.5f }, { 18, 0.5f } } };
                macros[2] = { .displayName = "Wave Position", .modifiers = { { 0, 1.0f }, { 1, 1.0f } } };
                macros[3] = { .displayName = "Cutoff", .modifiers = { { 12, 1.0f } }, .bipolar = true };
                macros[4] = { .displayName = "Resonance", .modifiers = { { 13, 0.5f } } };
                macros[5] = { .displayName = "Filter Env", .modifiers = { { 14, 0.5f } }, .bipolar = true };
            };
            metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kWaveSynth);
        }
        metadata.outputChannels = Patch::Metadata::OutputChannels::kStereo;
    }

  private:
    static constexpr size_t kBlock = 128;

    template<int I>
    struct SetParam
    {
        void operator()(CeSynthVoice& voice, Patch::optional_paramvalue_t value) const
        {
            if (value)
            {
                voice.p[I] = *value;
            }
        }
    };

    template<size_t... I>
    static void addAll(sdk::ParamHelper<CeSynthVoice>& helper, std::index_sequence<I...>)
    {
        (helper.addParam(static_cast<int>(I), makeParam(static_cast<int>(I)), SetParam<static_cast<int>(I)>{}), ...);
    }

    static Patch::Metadata::Param makeParam(int id)
    {
        const Def& d = (Engine == CESYN_PHASE ? phaseDefs() : waveDefs())[id];
        return Patch::Metadata::Param{
            .displayName = d.name,
            .defaultValue = d.def,
            .minValue = d.lo,
            .maxValue = d.hi,
            .editCurve = d.curve,
            .macroOperation = d.op,
            .isEnum = d.fmt == Fmt::kList,
            .valueFormatter = makeFormatter(d),
        };
    }

    static const std::array<Def, kParamCount>& waveDefs()
    {
        static const std::array<Def, kParamCount> defs = makeWaveDefs();
        return defs;
    }

    static const std::array<Def, kParamCount>& phaseDefs()
    {
        static const std::array<Def, kParamCount> defs = makePhaseDefs();
        return defs;
    }

    float p[kParamCount] = {};
    std::unique_ptr<uint8_t[]> engine;
    std::unique_ptr<uint8_t[]> fx;
};

} // namespace

std::unique_ptr<InstrumentType> createWaveSynth()
{
    return std::make_unique<InstrumentType>(InstrumentType{
      .name = "WAVE",
      .createMetadata = &CeSynthVoice<CESYN_WAVE>::createMetadata,
      .createVoice = [](int voiceIdx) -> std::unique_ptr<InstrumentVoice>
      { return std::make_unique<CeSynthVoice<CESYN_WAVE>>(voiceIdx); } });
}

std::unique_ptr<InstrumentType> createPhaseSynth()
{
    return std::make_unique<InstrumentType>(InstrumentType{
      .name = "PHASE",
      .createMetadata = &CeSynthVoice<CESYN_PHASE>::createMetadata,
      .createVoice = [](int voiceIdx) -> std::unique_ptr<InstrumentVoice>
      { return std::make_unique<CeSynthVoice<CESYN_PHASE>>(voiceIdx); } });
}

} // namespace polysynth

#endif
