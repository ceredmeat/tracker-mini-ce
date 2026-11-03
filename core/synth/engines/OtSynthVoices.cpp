// tracker-mini-ce: the host side for OpenTracker's synth engine modules.
//
// OpenTracker (github.com/opentracker-fw/opentracker-synths, MIT, Copyright (c) 2026
// Marcin Spoczynski) engines implement ot::audio::ISynthEngine and describe their
// parameters with an ot::mod::SynthDescriptor. This file wraps each one as a CE synth SDK
// engine: one CustomSynthVoice per voice, a slot object shared by the voices of one synth
// slot (the engine's slot scratch, the parameter values, the block counter), and CE
// metadata built from the descriptor. The module sources in core/synth/opentracker/ are
// unchanged; core/synth/audio/ and core/synth/modules/ stand in for OpenTracker's core
// headers.

#include "config.h"

#ifdef FEATURE_SYNTH

#include "OtSynthVoices.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"
#include "audio/synth_engine.h"
#include "modules/synth_descriptor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace ot
{
namespace audio
{
namespace fm4
{
ISynthEngine* engine();
}
namespace ju6
{
ISynthEngine* engine();
}
namespace dtype
{
ISynthEngine* engine();
}
namespace atype
{
ISynthEngine* engine();
}
namespace cstype
{
ISynthEngine* engine();
}
namespace ttype
{
ISynthEngine* engine();
}
} // namespace audio
namespace mod
{
extern const SynthDescriptor kFm4Descriptor;
extern const SynthDescriptor kJu6Descriptor;
extern const SynthDescriptor kDtypeDescriptor;
extern const SynthDescriptor kAtypeDescriptor;
extern const SynthDescriptor kCstypeDescriptor;
extern const SynthDescriptor kTtypeDescriptor;
} // namespace mod
} // namespace ot

namespace polysynth
{
namespace
{

using ot::audio::ISynthEngine;
using ot::mod::SynthDescriptor;

struct OtEngineDef
{
    ISynthEngine* (*engine)();
    const SynthDescriptor* desc;
    InstrumentTypeId typeId;
    const char* name;
};

const OtEngineDef kOtEngines[] = {
    { &ot::audio::fm4::engine, &ot::mod::kFm4Descriptor, InstrumentTypeId::kOtFm4Synth, "FM4" },
    { &ot::audio::ju6::engine, &ot::mod::kJu6Descriptor, InstrumentTypeId::kOtJu6Synth, "JU6" },
    { &ot::audio::dtype::engine, &ot::mod::kDtypeDescriptor, InstrumentTypeId::kOtDtypeSynth, "D-Type" },
    { &ot::audio::atype::engine, &ot::mod::kAtypeDescriptor, InstrumentTypeId::kOtAtypeSynth, "A-Type" },
    { &ot::audio::cstype::engine, &ot::mod::kCstypeDescriptor, InstrumentTypeId::kOtCstypeSynth, "CS-Type" },
    { &ot::audio::ttype::engine, &ot::mod::kTtypeDescriptor, InstrumentTypeId::kOtTtypeSynth, "T-Type" },
};
constexpr int kOtEngineCount = static_cast<int>(std::size(kOtEngines));

enum class OtParamId : int
{
    kFirst = 0,
    kLast = 127,
};
constexpr int kMaxParams = 128;

// The synth slot index (0..2) for engines that keep per-slot lines (JU6's chorus).
uint8_t gSlotUsed[ot::audio::kSynthSlots] = {};

// What the voices of one synth slot share.
struct OtSlot
{
    ot::audio::SynthSlotScratch scratch;
    float params[kMaxParams] = {};
    int count = 0;
    uint32_t serial = 1;
    uint32_t block = 0;
    uint32_t prepared = 0xFFFFFFFFu;
    int index = 0;
    bool ownsIndex = false;

    OtSlot()
    {
        for (int i = 0; i < ot::audio::kSynthSlots; i++)
        {
            if (!gSlotUsed[i])
            {
                gSlotUsed[i] = 1;
                index = i;
                ownsIndex = true;
                break;
            }
        }
    }
    ~OtSlot()
    {
        if (ownsIndex)
        {
            gSlotUsed[index] = 0;
        }
    }
};

// The slot made by the latest voice 0 of each engine: voices 1.. of the same synth
// slot are created right after it and join it.
std::weak_ptr<OtSlot> gLatestSlot[kOtEngineCount];

std::string fmtNumber(const char* f, float v)
{
    char buf[24];
    snprintf(buf, sizeof buf, f, static_cast<double>(v));
    return buf;
}

std::function<std::string(const Patch&, Patch::paramvalue_t)> makeFormatter(const ot::synthui::ParamUi& ui)
{
    using namespace ot::synthui;
    switch (ui.fmt)
    {
        case kFmtEnum:
        {
            const char* const* names = ui.names;
            int count = ui.count;
            return [names, count](const Patch&, Patch::paramvalue_t v)
            { return std::string(names[std::clamp<int>(std::lround(v), 0, count - 1)]); };
        }
        case kFmtPct: return sdk::createPercentFormatter();
        case kFmtPctMax:
            return [](const Patch&, Patch::paramvalue_t v)
            {
                int pc = static_cast<int>(std::lround(v * 100.0f));
                return pc > 0 ? fmtNumber("+%.0f %%", static_cast<float>(pc)) : fmtNumber("%.0f %%", static_cast<float>(pc));
            };
        case kFmtSec: return sdk::createTimeFormatter();
        case kFmtOnOff:
            return [](const Patch&, Patch::paramvalue_t v) { return std::string(v >= 0.5f ? "On" : "Off"); };
        case kFmtHz: return sdk::createFrequencyFormatter();
        case kFmtLfoHz:
            return [](const Patch&, Patch::paramvalue_t v) { return fmtNumber(v < 10.0f ? "%.2f Hz" : "%.1f Hz", v); };
        case kFmtSemi: return sdk::createSemitoneFormatter();
        case kFmtCents: return sdk::createCentsFormatter();
        case kFmtDb: return [](const Patch&, Patch::paramvalue_t v) { return fmtNumber("%.1f dB", v); };
        case kFmtMs10: return [](const Patch&, Patch::paramvalue_t v) { return fmtNumber("%.0f ms", v * 10.0f); };
        case kFmtPan:
            return [](const Patch&, Patch::paramvalue_t v)
            {
                int pc = static_cast<int>(std::lround(v * 100.0f));
                if (pc == 0)
                {
                    return std::string("C");
                }
                return pc < 0 ? fmtNumber("L%.0f", static_cast<float>(-pc)) : fmtNumber("R%.0f", static_cast<float>(pc));
            };
        case kFmtRatio: return [](const Patch&, Patch::paramvalue_t v) { return fmtNumber("%.2f", v); };
        case kFmtVal100: return [](const Patch&, Patch::paramvalue_t v) { return fmtNumber("%.0f", v * 100.0f); };
        case kFmtInt:
        default: return [](const Patch&, Patch::paramvalue_t v) { return fmtNumber("%.0f", v); };
    }
}

template<int K>
class OtVoice : public sdk::CustomSynthVoice<OtVoice<K>, OtParamId>
{
    using Base = sdk::CustomSynthVoice<OtVoice<K>, OtParamId>;

  public:
    explicit OtVoice(int voiceIdx)
      : voiceIdx(voiceIdx)
    {
        const SynthDescriptor& d = *kOtEngines[K].desc;
        std::shared_ptr<OtSlot> s = voiceIdx == 0 ? nullptr : gLatestSlot[K].lock();
        if (!s)
        {
            s = std::make_shared<OtSlot>();
            s->count = d.count;
            for (int i = 0; i < d.count && i < kMaxParams; i++)
            {
                s->params[i] = d.params[i].def;
            }
            engine()->slotReset(s->scratch);
            gLatestSlot[K] = s;
        }
        slot = std::move(s);
        std::memset(sv.state, 0, sizeof sv.state);
        sv.slotScratch = &slot->scratch;
        sv.slot = slot->index;
    }

    bool isActive() final { return active; }

    void onNoteOn(const InstrumentVoice::InstrumentVoiceContext&, note_t note, float velocity, bool isLegatoStep) final
    {
        ot::audio::SynthParams p = params();
        ensurePrepared(p);
        ot::audio::SynthNote n;
        n.pitch = static_cast<float>(note);
        n.note = note;
        n.velocity = velocity;
        n.reused = active;
        n.overlap = isLegatoStep && active;
        n.fxGlide = false;
        noteVelocity = velocity;
        engine()->noteOn(sv, p, n);
        active = true;
    }

    void onNoteOff(const InstrumentVoice::InstrumentVoiceContext&, note_t) final
    {
        ot::audio::SynthParams p = params();
        engine()->noteOff(sv, p);
    }

    void renderVoiceAudio(const InstrumentVoice::InstrumentVoiceContext& ctx,
                          std::optional<AudioBufferStereoRef>,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef>) final
    {
        float* L = outputBuffer[0].data();
        float* R = outputBuffer[1].data();
        std::fill(L, L + ot::audio::kBlock, 0.0f);
        std::fill(R, R + ot::audio::kBlock, 0.0f);
        if (voiceIdx == 0)
        {
            slot->block++; // voice 0 counts the slot's blocks
        }
        engine()->setTempo(ctx.tempo);
        ot::audio::SynthParams p = params();
        ensurePrepared(p);
        sv.bendStart = sv.bend;
        sv.bend = this->pitchBendSemitones;
        if (!active)
        {
            lastGain = 0.0f;
            return;
        }
        const SynthDescriptor& d = *kOtEngines[K].desc;
        float vol = d.volume >= 0 ? slot->params[d.volume] : 1.0f;
        float g = std::clamp(vol, 0.0f, 4.0f) * noteVelocity;
        if (!gainValid)
        {
            lastGain = g;
            gainValid = true;
        }
        float gInc = (g - lastGain) / static_cast<float>(ot::audio::kBlock);
        active = engine()->render(sv, p, lastGain, gInc, L, R);
        lastGain = g;
        if (!active)
        {
            gainValid = false;
        }
    }

    void renderPostProcessAudio(const InstrumentVoice::InstrumentVoiceContext&, AudioBufferStereoRef outputBuffer) final
    {
        ot::audio::SynthParams p = params();
        engine()->slotPost(slot->index, slot->scratch, p, outputBuffer[0].data(), outputBuffer[1].data());
    }

    static void createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
    {
        using EditorSection = Patch::Metadata::EditorLayout::EditorSection;
        using EditorElement = Patch::Metadata::EditorLayout::EditorElement;
        using Curve = Patch::Metadata::Param::Curve;
        const SynthDescriptor& d = *kOtEngines[K].desc;
        const int count = std::min<int>(d.count, kMaxParams);

        metadata.params.resize(count);
        metadata.params.shrink_to_fit();
        if (Base::paramHandlers.size() < static_cast<size_t>(count))
        {
            Base::paramHandlers.resize(count);
        }
        for (int i = 0; i < count; i++)
        {
            const auto& r = d.params[i];
            const auto& ui = d.ui[i];
            Curve curve = Curve::kLinear;
            if (ui.fmt == ot::synthui::kFmtSec)
            {
                curve = Curve::kPower;
            }
            else if ((ui.fmt == ot::synthui::kFmtHz || ui.fmt == ot::synthui::kFmtLfoHz) && r.min > 0.0f)
            {
                curve = Curve::kExponential;
            }
            bool isEnum = ui.fmt == ot::synthui::kFmtEnum || ui.fmt == ot::synthui::kFmtOnOff ||
                          ui.fmt == ot::synthui::kFmtInt || ui.fmt == ot::synthui::kFmtSemi;
            metadata.params[i] = Patch::Metadata::Param{
                .displayName = ui.name,
                .defaultValue = r.def,
                .minValue = r.min,
                .maxValue = r.max,
                .editCurve = curve,
                .isEnum = isEnum,
                .valueFormatter = makeFormatter(ui),
            };
            Base::paramHandlers[i] = handlerFor(i);
        }

        std::vector<EditorSection> sections;
        for (int s = 0; s < d.sectionCount; s++)
        {
            const auto& sec = d.sections[s];
            if (!sec.params || !sec.count)
            {
                continue; // the macro sections' marker
            }
            std::vector<EditorElement> elems;
            for (int k = 0; k < sec.count; k++)
            {
                elems.push_back(EditorElement{ .type = EditorElement::Type::kParam, .id = sec.params[k] });
            }
            if (sections.size() < 6)
            {
                sections.push_back(EditorSection{ .name = sec.name, .shortName = sec.shortName, .elements = elems });
            }
            else
            {
                auto& last = sections.back().elements; // more than six: onto the sixth
                last.insert(last.end(), elems.begin(), elems.end());
            }
        }
        metadata.layoutFlat.sections = std::move(sections);
        metadata.layout23 = metadata.layoutFlat;
        metadata.instrumentTypeId = static_cast<int>(kOtEngines[K].typeId);
        metadata.outputChannels = Patch::Metadata::OutputChannels::kStereo;
    }

  private:
    static ISynthEngine* engine() { return kOtEngines[K].engine(); }

    using HandlerFn = void (*)(OtVoice&, Patch::optional_paramvalue_t);

    template<int I>
    static void setParamAt(OtVoice& v, Patch::optional_paramvalue_t value)
    {
        if (value)
        {
            v.setSlotParam(I, *value);
        }
    }

    template<size_t... I>
    static constexpr std::array<HandlerFn, sizeof...(I)> makeHandlers(std::index_sequence<I...>)
    {
        return { &setParamAt<static_cast<int>(I)>... };
    }

    static HandlerFn handlerFor(int i)
    {
        static const std::array<HandlerFn, kMaxParams> handlers =
          makeHandlers(std::make_index_sequence<kMaxParams>{});
        return handlers[i];
    }

    ot::audio::SynthParams params() const
    {
        ot::audio::SynthParams p;
        p.start = slot->params;
        p.end = slot->params;
        p.count = slot->count;
        p.serial = slot->serial;
        p.moving = false;
        return p;
    }

    void ensurePrepared(const ot::audio::SynthParams& p)
    {
        if (slot->prepared != slot->block)
        {
            engine()->prepare(slot->scratch, p);
            slot->prepared = slot->block;
        }
    }

  public:
    // Parameter changes: the CE SDK calls the handler with the voice; the value goes to
    // the slot (all voices of a slot share it).
    void setSlotParam(int id, float value)
    {
        if (id >= 0 && id < slot->count && slot->params[id] != value)
        {
            slot->params[id] = value;
            slot->serial++;
        }
    }

  private:
    int voiceIdx;
    std::shared_ptr<OtSlot> slot;
    ot::audio::SynthVoice sv;
    bool active = false;
    bool gainValid = false;
    float lastGain = 0.0f;
    float noteVelocity = 1.0f;
};

template<int K>
std::unique_ptr<InstrumentType> makeOtType()
{
    return std::make_unique<InstrumentType>(InstrumentType{
      .name = kOtEngines[K].name,
      .createMetadata = &OtVoice<K>::createMetadata,
      .createVoice = [](int voiceIdx) -> std::unique_ptr<InstrumentVoice>
      { return std::make_unique<OtVoice<K>>(voiceIdx); } });
}

} // namespace

std::unique_ptr<InstrumentType> createOtFm4Synth() { return makeOtType<0>(); }
std::unique_ptr<InstrumentType> createOtJu6Synth() { return makeOtType<1>(); }
std::unique_ptr<InstrumentType> createOtDtypeSynth() { return makeOtType<2>(); }
std::unique_ptr<InstrumentType> createOtAtypeSynth() { return makeOtType<3>(); }
std::unique_ptr<InstrumentType> createOtCstypeSynth() { return makeOtType<4>(); }
std::unique_ptr<InstrumentType> createOtTtypeSynth() { return makeOtType<5>(); }

} // namespace polysynth

#endif
