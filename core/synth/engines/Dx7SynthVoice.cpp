// tracker-mini-ce: DX7, a six-operator FM engine on Google's Music Synthesizer for All
// (msfa, Apache-2.0, core/synth/msfa/; the DX7 core Dexed is built on - Dexed itself is
// GPL and not used). Every DX7 voice parameter is a patch parameter, in the DX7's own
// order and ranges (ids 0-144: the 155-byte voice without the name; 145: Volume).
//
// The voice is CE's InstrumentVoice directly (CustomSynthVoice tops out at 128
// parameters). Changes reach a sounding note at the next block (Dx7Note::update, added to
// msfa here), as do amplitude modulation and Osc Key Sync. msfa renders 64 frames at a
// time, so a CE block is two of them.

#include "config.h"

#ifdef FEATURE_SYNTH

#include "Dx7SynthVoice.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"

#include "msfa/synth.h"
#include "msfa/controllers.h"
#include "msfa/dx7note.h"
#include "msfa/exp2.h"
#include "msfa/freqlut.h"
#include "msfa/lfo.h"
#include "msfa/pitchenv.h"
#include "msfa/sin.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace polysynth
{
namespace
{

constexpr int kVoiceBytes = 145; // the DX7 voice parameters (VCED 0-144, no name)
constexpr int kVolume = 145;
constexpr int kParamCount = 146;
constexpr int kMsfaBlock = 64; // msfa's N

// DX7 voice parameter layout: op k (DX7 operator 6 - k... see opBase), 21 each, then the
// global ones.
enum OpField : int
{
    kR1 = 0, kR2, kR3, kR4, kL1, kL2, kL3, kL4, kBreak, kLDepth, kRDepth, kLCurve, kRCurve,
    kRateScale, kAms, kVelSens, kLevel, kMode, kCoarse, kFine, kDetune, kOpFields
};
enum Global : int
{
    kPR1 = 126, kPR2, kPR3, kPR4, kPL1, kPL2, kPL3, kPL4, kAlgorithm, kFeedback, kOscSync,
    kLfoSpeed, kLfoDelay, kLfoPmd, kLfoAmd, kLfoSync, kLfoWave, kPms, kTranspose
};
// DX7 operator n (1..6) is stored at block 6 - n.
constexpr int opBase(int n) { return (6 - n) * kOpFields; }

const char* const kCurveNames[4] = { "-LIN", "-EXP", "+EXP", "+LIN" };
const char* const kModeNames[2] = { "Ratio", "Fixed" };
const char* const kOnOff[2] = { "Off", "On" };
const char* const kWaveNames[6] = { "Triangle", "Saw Down", "Saw Up", "Square", "Sine", "S&H" };
const char* const kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

enum class Fmt : uint8_t
{
    kNum,     // 0..99 etc.
    kList,
    kPlusOne, // shown from 1 (Algorithm)
    kDetune,  // 0..14 shown -7..+7
    kNote,    // Break Point: A-1 .. C8
    kTranspose,
    kPercent,
};

struct Def
{
    const char* name;
    float def, max;
    Fmt fmt = Fmt::kNum;
    const char* const* list = nullptr;
};

std::array<Def, kParamCount> makeDefs()
{
    std::array<Def, kParamCount> d{};
    static const char* const kOpNames[6][kOpFields] = {
#define OPN(n)                                                                                         \
    { n " R1",     n " R2",     n " R3",      n " R4",     n " L1",    n " L2",     n " L3",           \
      n " L4",     n " Break",  n " L Depth", n " R Depth", n " L Curve", n " R Curve", n " Rate Scl", \
      n " AMS",    n " Vel Sens", n " Level", n " Mode",   n " Coarse", n " Fine",   n " Detune" }
        OPN("Op6"), OPN("Op5"), OPN("Op4"), OPN("Op3"), OPN("Op2"), OPN("Op1"),
#undef OPN
    };
    for (int k = 0; k < 6; k++)
    {
        const int b = k * kOpFields;
        const bool carrier = k == 5; // DX7 INIT VOICE: only operator 1 sounds
        const char* const* n = kOpNames[k];
        d[b + kR1] = { n[kR1], 99, 99 };
        d[b + kR2] = { n[kR2], 99, 99 };
        d[b + kR3] = { n[kR3], 99, 99 };
        d[b + kR4] = { n[kR4], 99, 99 };
        d[b + kL1] = { n[kL1], 99, 99 };
        d[b + kL2] = { n[kL2], 99, 99 };
        d[b + kL3] = { n[kL3], 99, 99 };
        d[b + kL4] = { n[kL4], 0, 99 };
        d[b + kBreak] = { n[kBreak], 39, 99, Fmt::kNote };
        d[b + kLDepth] = { n[kLDepth], 0, 99 };
        d[b + kRDepth] = { n[kRDepth], 0, 99 };
        d[b + kLCurve] = { n[kLCurve], 0, 3, Fmt::kList, kCurveNames };
        d[b + kRCurve] = { n[kRCurve], 0, 3, Fmt::kList, kCurveNames };
        d[b + kRateScale] = { n[kRateScale], 0, 7 };
        d[b + kAms] = { n[kAms], 0, 3 };
        d[b + kVelSens] = { n[kVelSens], 0, 7 };
        d[b + kLevel] = { n[kLevel], carrier ? 99.0f : 0.0f, 99 };
        d[b + kMode] = { n[kMode], 0, 1, Fmt::kList, kModeNames };
        d[b + kCoarse] = { n[kCoarse], 1, 31 };
        d[b + kFine] = { n[kFine], 0, 99 };
        d[b + kDetune] = { n[kDetune], 7, 14, Fmt::kDetune };
    }
    d[kPR1] = { "Pitch R1", 99, 99 };
    d[kPR2] = { "Pitch R2", 99, 99 };
    d[kPR3] = { "Pitch R3", 99, 99 };
    d[kPR4] = { "Pitch R4", 99, 99 };
    d[kPL1] = { "Pitch L1", 50, 99 };
    d[kPL2] = { "Pitch L2", 50, 99 };
    d[kPL3] = { "Pitch L3", 50, 99 };
    d[kPL4] = { "Pitch L4", 50, 99 };
    d[kAlgorithm] = { "Algorithm", 0, 31, Fmt::kPlusOne };
    d[kFeedback] = { "Feedback", 0, 7 };
    d[kOscSync] = { "Osc Sync", 1, 1, Fmt::kList, kOnOff };
    d[kLfoSpeed] = { "LFO Speed", 35, 99 };
    d[kLfoDelay] = { "LFO Delay", 0, 99 };
    d[kLfoPmd] = { "LFO PMD", 0, 99 };
    d[kLfoAmd] = { "LFO AMD", 0, 99 };
    d[kLfoSync] = { "LFO Sync", 1, 1, Fmt::kList, kOnOff };
    d[kLfoWave] = { "LFO Wave", 0, 5, Fmt::kList, kWaveNames };
    d[kPms] = { "Pitch Mod Sens", 3, 7 };
    d[kTranspose] = { "Transpose", 24, 48, Fmt::kTranspose };
    d[kVolume] = { "Volume", 0.8f, 1.5f, Fmt::kPercent };
    return d;
}

const std::array<Def, kParamCount>& defs()
{
    static const std::array<Def, kParamCount> d = makeDefs();
    return d;
}

std::string noteName(int n) // DX7 key 0 = A-1
{
    int midi = n + 21;
    char buf[8];
    snprintf(buf, sizeof buf, "%s%d", kNoteNames[midi % 12], midi / 12 - 2);
    return buf;
}

std::function<std::string(const Patch&, Patch::paramvalue_t)> formatterOf(const Def& d)
{
    switch (d.fmt)
    {
        case Fmt::kList:
        {
            const char* const* list = d.list;
            int max = static_cast<int>(d.max);
            return [list, max](const Patch&, Patch::paramvalue_t v)
            { return std::string(list[std::clamp<int>(std::lround(v), 0, max)]); };
        }
        case Fmt::kPlusOne:
            return [](const Patch&, Patch::paramvalue_t v) { return std::to_string(std::lround(v) + 1); };
        case Fmt::kDetune:
            return [](const Patch&, Patch::paramvalue_t v)
            {
                long x = std::lround(v) - 7;
                return x > 0 ? "+" + std::to_string(x) : std::to_string(x);
            };
        case Fmt::kNote: return [](const Patch&, Patch::paramvalue_t v) { return noteName(std::lround(v)); };
        case Fmt::kTranspose:
            return [](const Patch&, Patch::paramvalue_t v)
            {
                long x = std::lround(v) - 24;
                return x > 0 ? "+" + std::to_string(x) + " st" : std::to_string(x) + " st";
            };
        case Fmt::kPercent: return sdk::createPercentFormatter();
        case Fmt::kNum:
        default: return [](const Patch&, Patch::paramvalue_t v) { return std::to_string(std::lround(v)); };
    }
}

void initTables()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    Exp2::init();
    Sin::init();
    Freqlut::init(sdk::kSampleRate);
    Lfo::init(sdk::kSampleRate);
    PitchEnv::init(sdk::kSampleRate);
    done = true;
}

class Dx7Voice final : public InstrumentVoice
{
  public:
    Dx7Voice()
    {
        initTables();
        const auto& d = defs();
        for (int i = 0; i < kParamCount; i++)
        {
            p[i] = d[i].def;
        }
    }

    void onMidiEvent(const InstrumentVoiceContext&, const MidiEvent& e) override
    {
        switch (e.type)
        {
            case MidiEvent::Type::kNoteOn:
                noteOn(e.data.note.note, e.data.note.velocity);
                break;
            case MidiEvent::Type::kNoteOff:
                if (playing && e.data.note.note == note)
                {
                    noteOff();
                }
                break;
            case MidiEvent::Type::kPitchBendChange:
            {
                // msfa: +-0x2000 = +-3 semitones; CE: +-2
                float semis = (static_cast<float>(e.data.pitchBendChange.value) * 2.0f) /
                              static_cast<float>(MidiEvent::Data::PitchBendChange::kMaxValue);
                pitch = std::clamp(0x2000 + static_cast<int>(semis / 3.0f * 8192.0f), 0, 16383);
                break;
            }
            default: break;
        }
    }

    void onParamChange(int id, Patch::optional_paramvalue_t v) override
    {
        if (id >= 0 && id < kParamCount && v)
        {
            if (p[id] != *v)
            {
                p[id] = *v;
                if (id < kVoiceBytes)
                {
                    dirty = true; // a sounding note takes it at the next block
                }
            }
        }
    }

    void renderVoiceAudio(const InstrumentVoiceContext&,
                          std::optional<AudioBufferStereoRef>,
                          AudioBufferStereoRef out,
                          std::span<AudioBufferRef>) override
    {
        auto L = out[0];
        auto R = out[1];
        if (!active)
        {
            std::fill(L.begin(), L.end(), 0.0f);
            std::fill(R.begin(), R.end(), 0.0f);
            return;
        }
        if (dirty)
        {
            char voice[156];
            fillVoice(voice);
            if (voice[kLfoSpeed] != lfoParams[0] || std::memcmp(lfoParams, &voice[kLfoSpeed], 6) != 0)
            {
                std::memcpy(lfoParams, &voice[kLfoSpeed], 6);
                lfo.reset(lfoParams); // keeps its phase
            }
            dx7.update(voice, midiNote, midiVelocity);
            dirty = false;
        }
        const float gain = std::clamp(p[kVolume], 0.0f, 1.5f) * (1.0f / static_cast<float>(1 << 26)); // one carrier at full level: 0.5
        int32_t peak = 0;
        for (size_t at = 0; at + kMsfaBlock <= L.size(); at += kMsfaBlock)
        {
            int32_t buf[kMsfaBlock] = {};
            int32_t lfoVal = lfo.getsample();
            int32_t lfoDelay = lfo.getdelay();
            sharedCtrls().values_[kControllerPitch] = pitch;
            dx7.compute(buf, lfoVal, lfoDelay, &sharedCtrls());
            for (int i = 0; i < kMsfaBlock; i++)
            {
                const float s = static_cast<float>(buf[i]) * gain;
                L[at + i] = s;
                R[at + i] = s;
                int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
                peak = a > peak ? a : peak;
            }
        }
        // A released note ends when it has been silent (below about -84 dB) for a while.
        if (!playing)
        {
            quietBlocks = peak < (1 << 14) ? quietBlocks + 1 : 0;
            if (quietBlocks > 8)
            {
                active = false;
            }
        }
    }

    bool isActive() override { return active; }
    bool handlesNote(note_t n) override { return playing && n == note; }
    bool handlesAnyNote() override { return playing; }
    tiny_optional<note_t> getLastNote() override { return hasNote ? tiny_optional<note_t>(note) : std::nullopt; }
    tiny_optional<float> getCurrentNoteVelocity() override
    {
        return playing ? tiny_optional<float>(velocity) : std::nullopt;
    }

    static void createMetadata(const Patch&, Patch::Metadata& m)
    {
        using EditorSection = Patch::Metadata::EditorLayout::EditorSection;
        using EditorElement = Patch::Metadata::EditorLayout::EditorElement;
        using Param = Patch::Metadata::Param;
        const auto& d = defs();
        m.params.resize(kParamCount);
        m.params.shrink_to_fit();
        for (int i = 0; i < kParamCount; i++)
        {
            m.params[i] = Param{
                .displayName = d[i].name,
                .defaultValue = d[i].def,
                .minValue = 0.0f,
                .maxValue = d[i].max,
                .isEnum = d[i].fmt != Fmt::kPercent,
                .valueFormatter = formatterOf(d[i]),
            };
        }
        auto range = [](int from, int to)
        {
            std::vector<EditorElement> v;
            for (int i = from; i <= to; i++)
            {
                v.push_back(EditorElement{ .type = EditorElement::Type::kParam, .id = i });
            }
            return v;
        };
        std::vector<EditorSection> s;
        s.push_back(EditorSection{ .name = "Voice", .shortName = "Voice",
                                   .elements = { EditorElement{ .type = EditorElement::Type::kParam, .id = kAlgorithm },
                                                 EditorElement{ .type = EditorElement::Type::kParam, .id = kFeedback },
                                                 EditorElement{ .type = EditorElement::Type::kParam, .id = kOscSync },
                                                 EditorElement{ .type = EditorElement::Type::kParam, .id = kTranspose },
                                                 EditorElement{ .type = EditorElement::Type::kParam, .id = kVolume } } });
        for (int n = 1; n <= 6; n++)
        {
            static const char* const kNames[6] = { "Operator 1", "Operator 2", "Operator 3",
                                                   "Operator 4", "Operator 5", "Operator 6" };
            static const char* const kShort[6] = { "Op 1", "Op 2", "Op 3", "Op 4", "Op 5", "Op 6" };
            s.push_back(EditorSection{ .name = kNames[n - 1], .shortName = kShort[n - 1],
                                       .elements = range(opBase(n), opBase(n) + kOpFields - 1) });
        }
        s.push_back(EditorSection{ .name = "Modulation", .shortName = "Mod",
                                   .elements = range(kPR1, kPL4) });
        auto& mod = s.back().elements;
        for (int i : { kLfoSpeed, kLfoDelay, kLfoPmd, kLfoAmd, kLfoSync, kLfoWave, kPms })
        {
            mod.push_back(EditorElement{ .type = EditorElement::Type::kParam, .id = i });
        }
        m.layoutFlat.sections = std::move(s);
        m.layout23 = m.layoutFlat;
        m.initDefaultMacros = [](std::vector<Patch::Metadata::Macro>& macros)
        {
            macros[0] = { .displayName = "Op1 Level", .modifiers = { { opBase(1) + kLevel, 0.5f } } };
            macros[1] = { .displayName = "Op2 Level", .modifiers = { { opBase(2) + kLevel, 0.5f } } };
            macros[2] = { .displayName = "Feedback", .modifiers = { { kFeedback, 0.5f } } };
            macros[3] = { .displayName = "LFO Speed", .modifiers = { { kLfoSpeed, 0.5f } } };
            macros[4] = { .displayName = "Vibrato", .modifiers = { { kLfoPmd, 0.5f } } };
            macros[5] = { .displayName = "Volume", .modifiers = { { kVolume, 0.5f } } };
        };
        m.instrumentTypeId = static_cast<int>(InstrumentTypeId::kDx7Synth);
        m.outputChannels = Patch::Metadata::OutputChannels::kStereo;
    }

  private:
    void fillVoice(char* voice) const
    {
        for (int i = 0; i < kVoiceBytes; i++)
        {
            voice[i] = static_cast<char>(std::clamp<long>(std::lround(p[i]), 0, std::lround(defs()[i].max)));
        }
        for (int i = kVoiceBytes; i < 156; i++)
        {
            voice[i] = ' ';
        }
    }

    void noteOn(int n, int vel)
    {
        char voice[156];
        fillVoice(voice);
        midiNote = std::clamp(n + voice[kTranspose] - 24, 0, 127);
        midiVelocity = std::clamp(vel, 1, 127);
        dx7.init(voice, midiNote, midiVelocity);
        std::memcpy(lfoParams, &voice[kLfoSpeed], 6);
        lfo.reset(lfoParams);
        lfo.keydown();
        dirty = false;
        note = static_cast<note_t>(n);
        velocity = static_cast<float>(vel) / 127.0f;
        hasNote = playing = active = true;
        quietBlocks = 0;
    }

    void noteOff()
    {
        dx7.keyup();
        playing = false;
    }

    // msfa reads only the pitch bend from its controllers: one shared object (the voices
    // render one after another), each voice setting its bend before it computes.
    static Controllers& sharedCtrls()
    {
        static Controllers c{};
        return c;
    }

    float p[kParamCount] = {};
    Dx7Note dx7;
    Lfo lfo;
    int pitch = 0x2000;
    int midiNote = 60;
    int midiVelocity = 100;
    char lfoParams[6] = {};
    bool dirty = false;
    note_t note = 60;
    float velocity = 1.0f;
    bool hasNote = false;
    bool playing = false;
    bool active = false;
    int quietBlocks = 0;
};

} // namespace

std::unique_ptr<InstrumentType> createDx7Synth()
{
    return std::make_unique<InstrumentType>(InstrumentType{
      .name = "DX7",
      .createMetadata = &Dx7Voice::createMetadata,
      .createVoice = [](int) -> std::unique_ptr<InstrumentVoice> { return std::make_unique<Dx7Voice>(); } });
}

} // namespace polysynth

#endif
