// tracker-mini-ce: OpenTracker compatibility layer (see audio/synth_engine.h): the
// descriptor a synth module gives for its parameters, Edit Patch labels and sections.
#pragma once

#include <cstdint>

namespace ot {
namespace synthui {

enum Fmt : uint8_t
{
    kFmtPct = 0,   // 0..1 -> %
    kFmtPctMax,    // signed %
    kFmtSec,
    kFmtInt,
    kFmtOnOff,
    kFmtHz,
    kFmtLfoHz,
    kFmtSemi,
    kFmtCents,
    kFmtDb,
    kFmtMs10,      // tens of ms
    kFmtPan,
    kFmtRatio,
    kFmtVal100,    // 0..1 shown 0..100
    kFmtPercNote,
    kFmtBodyWave,
    kFmtEnum,
};

struct ParamUi
{
    const char* name;
    uint8_t fmt;
    const char* const* names;
    uint8_t count;
};

namespace names {
inline const char* const kGlide[4] = { "Always", "Overlap", "Legato", "Legato O." };
inline const char* const kOnOff[2] = { "Off", "On" };
} // namespace names

} // namespace synthui

namespace mod {

struct PatchParamRange
{
    float def, min, max;
};

struct Section
{
    const char* shortName;
    const char* name;
    const uint8_t* params;
    uint8_t count;
};

constexpr uint8_t kSynthFlagNotePriority = 1;
constexpr uint8_t kSynthFlagKitState = 2; // the slot scratch is a kit the voices share

struct SynthDescriptor
{
    int32_t engineId;
    const char* dir;
    uint8_t flags;
    int8_t volume; // the Volume parameter (the host applies it), or -1
    uint8_t count;
    const PatchParamRange* params;
    const synthui::ParamUi* ui;
    const Section* sections;
    uint8_t sectionCount;
    uint8_t extCount;
    const void* ext;
    const char* displayName = nullptr;
    int8_t priority = -1; // the note priority parameter, or -1
};

} // namespace mod
} // namespace ot

#define OT_SYNTH_ENUM(n) ot::synthui::kFmtEnum, n, uint8_t(sizeof(n) / sizeof(n[0]))
#define OT_SYNTH_SECTION(s, l, arr) ot::mod::Section{ s, l, arr, uint8_t(sizeof(arr)) }
#define OT_SYNTH_MACRO_SECTIONS ot::mod::Section{ nullptr, nullptr, nullptr, 0 }
