// T-Type drum synth descriptor (synth_descriptor.h): stored ranges, Edit
// Patch labels, formats, Character names and sections. Patches/TTYPE/,
// "T-Type" on the Synth page (displayName). The kit outlives a voice
// (kSynthFlagKitState); no Volume parameter (velocity applies per hit).
#include "ttype.h"
#include "modules/synth_descriptor.h"

namespace ot {
namespace mod {

namespace {

using namespace ot::synthui;
using namespace ot::audio::ttype;

// Init: a punchy hybrid kit (every Character 0 but the Bright cowbell).
const PatchParamRange kParams[kParamCount] = {
    {0.9f, 0, 1},       // Kick Level
    {55, 30, 120},      // Kick Tune (Hz)
    {0.35f, 0.05f, 2.5f},   // Kick Decay (s)
    {0.4f, 0, 1},       // Kick Attack (click)
    {0.5f, 0, 1},       // Kick Sweep
    {0.2f, 0, 1},       // Kick Drive
    {0, 0, 2},          // Kick Character: Punch / Boom / Knock
    {0.8f, 0, 1},       // Snare Level
    {185, 120, 400},    // Snare Tune (Hz)
    {0.2f, 0.05f, 1},   // Snare Decay (s)
    {0.5f, 0, 1},       // Snare Tone
    {0.5f, 0, 1},       // Snare Snappy
    {0, 0, 2},          // Snare Character: Crack / Soft / Thin
    {0.8f, 0, 1},       // Clap Level
    {0.5f, 0, 1},       // Clap Tone
    {0.25f, 0.05f, 1.5f},   // Clap Decay (s)
    {0.5f, 0, 1},       // Clap Tail
    {0, 0, 2},          // Clap Character: Wide / Dry / Tight
    {0.8f, 0, 1},       // Low Tom Level
    {0.8f, 0, 1},       // Mid Tom Level
    {0.8f, 0, 1},       // High Tom Level
    {90, 40, 200},      // Low Tom Tune (Hz)
    {130, 60, 300},     // Mid Tom Tune (Hz)
    {190, 80, 400},     // High Tom Tune (Hz)
    {0.4f, 0.05f, 2},   // Low Tom Decay (s)
    {0.35f, 0.05f, 2},  // Mid Tom Decay (s)
    {0.3f, 0.05f, 2},   // High Tom Decay (s)
    {0.3f, 0, 1},       // Tom Tone (noise attack)
    {0, 0, 2},          // Tom Character: Sweep / Round / Short
    {0.8f, 0, 1},       // Rim Level
    {1700, 500, 4000},  // Rim Tune (Hz)
    {0, 0, 2},          // Rim Character: Stick / Pop / Tick
    {0.7f, 0, 1},       // Bell Level
    {540, 300, 1200},   // Bell Tune (Hz)
    {0.3f, 0.05f, 1.5f},    // Bell Decay (s)
    {1, 0, 2},          // Bell Character: Clang / Bright / Low
    {0.8f, 0, 1},       // Hat Closed (level)
    {0.8f, 0, 1},       // Hat Open (level)
    {205, 100, 600},    // Hat Tune (Hz, the square bank)
    {0.06f, 0.01f, 0.5f},   // Hat Closed Decay (s)
    {0.5f, 0.1f, 3},    // Hat Open Decay (s)
    {0.5f, 0, 1},       // Hat Tone
    {0, 0, 2},          // Hat Character: Noisy / Metal / Thin
    {0.7f, 0, 1},       // Cymbal Level
    {1, 0.5f, 2},       // Cymbal Tune (x the bank's filters)
    {1.5f, 0.2f, 6},    // Cymbal Decay (s)
    {0.5f, 0, 1},       // Cymbal Tone
    {0, 0, 2},          // Cymbal Character: Crash / Ring / Thin
    // Sends 48-69: Delay, Reverb per sound.
    {0, 0, 1}, {0, 0, 1},       // Kick
    {0, 0, 1}, {0.3f, 0, 1},    // Snare
    {0, 0, 1}, {0.3f, 0, 1},    // Clap
    {0, 0, 1}, {0, 0, 1},       // Low Tom
    {0, 0, 1}, {0, 0, 1},       // Mid Tom
    {0, 0, 1}, {0, 0, 1},       // High Tom
    {0, 0, 1}, {0, 0, 1},       // Rim
    {0, 0, 1}, {0, 0, 1},       // Bell
    {0.2f, 0, 1}, {0, 0, 1},    // Hat
    {0, 0, 1}, {0, 0, 1},       // Crash
    {0, 0, 1}, {0, 0, 1},       // Ride
    {1, 0, 1},          // Accent
    {0, 0, 1},          // Drive
    {0.6f, 0, 1},       // Width
};

const char* const kKickCharName[3] = {"Punch", "Boom", "Knock"};
const char* const kSnareCharName[3] = {"Crack", "Soft", "Thin"};
const char* const kClapCharName[3] = {"Wide", "Dry", "Tight"};
const char* const kTomCharName[3] = {"Sweep", "Round", "Short"};
const char* const kRimCharName[3] = {"Stick", "Pop", "Tick"};
const char* const kBellCharName[3] = {"Clang", "Bright", "Low"};
const char* const kHatCharName[3] = {"Noisy", "Metal", "Thin"};
const char* const kCymCharName[3] = {"Crash", "Ring", "Thin"};

#define E(n) OT_SYNTH_ENUM(n)
#define F(f) f, nullptr, 0
#define SENDS(d) {d " Delay", F(kFmtPct)}, {d " Reverb", F(kFmtPct)}
const ParamUi kUi[kParamCount] = {
    {"Level", F(kFmtPct)},         {"Tune", F(kFmtHz)},          {"Decay", F(kFmtSec)},
    {"Attack", F(kFmtPct)},        {"Sweep", F(kFmtPct)},        {"Drive", F(kFmtPct)},
    {"Character", E(kKickCharName)},
    {"Level", F(kFmtPct)},         {"Tune", F(kFmtHz)},          {"Decay", F(kFmtSec)},
    {"Tone", F(kFmtPct)},          {"Snappy", F(kFmtPct)},       {"Character", E(kSnareCharName)},
    {"Level", F(kFmtPct)},         {"Tone", F(kFmtPct)},         {"Decay", F(kFmtSec)},
    {"Tail", F(kFmtPct)},          {"Character", E(kClapCharName)},
    {"Low Level", F(kFmtPct)},     {"Mid Level", F(kFmtPct)},    {"High Level", F(kFmtPct)},
    {"Low Tune", F(kFmtHz)},       {"Mid Tune", F(kFmtHz)},      {"High Tune", F(kFmtHz)},
    {"Low Decay", F(kFmtSec)},     {"Mid Decay", F(kFmtSec)},    {"High Decay", F(kFmtSec)},
    {"Tone", F(kFmtPct)},          {"Character", E(kTomCharName)},
    {"Rim Level", F(kFmtPct)},     {"Rim Tune", F(kFmtHz)},      {"Rim Character", E(kRimCharName)},
    {"Bell Level", F(kFmtPct)},    {"Bell Tune", F(kFmtHz)},     {"Bell Decay", F(kFmtSec)},
    {"Bell Character", E(kBellCharName)},
    {"Closed", F(kFmtPct)},        {"Open", F(kFmtPct)},         {"Tune", F(kFmtHz)},
    {"Closed Decay", F(kFmtSec)},  {"Open Decay", F(kFmtSec)},   {"Tone", F(kFmtPct)},
    {"Character", E(kHatCharName)},
    {"Level", F(kFmtPct)},         {"Tune", F(kFmtVal100)},      {"Decay", F(kFmtSec)},
    {"Tone", F(kFmtPct)},          {"Character", E(kCymCharName)},
    SENDS("Kick"), SENDS("Snare"), SENDS("Clap"), SENDS("Low Tom"), SENDS("Mid Tom"), SENDS("High Tom"),
    SENDS("Rim"), SENDS("Bell"), SENDS("Hat"), SENDS("Crash"), SENDS("Ride"),
    {"Accent", F(kFmtPct)},        {"Drive", F(kFmtPct)},        {"Width", F(kFmtPct)},
};
#undef SENDS
#undef E
#undef F

const uint8_t kKick[] = {kKickLevel, kKickTune, kKickDecay, kKickAttack, kKickSweep, kKickDrive, kKickChar};
const uint8_t kSnare[] = {kSnareLevel, kSnareTune, kSnareDecay, kSnareTone, kSnareSnappy, kSnareChar};
const uint8_t kClap[] = {kClapLevel, kClapTone, kClapDecay, kClapTail, kClapChar};
const uint8_t kToms[] = {kTomLoLevel, kTomMidLevel, kTomHiLevel, kTomLoTune, kTomMidTune, kTomHiTune,
                         kTomLoDecay, kTomMidDecay, kTomHiDecay, kTomTone, kTomChar};
const uint8_t kPerc[] = {kRimLevel, kRimTune, kRimChar, kBellLevel, kBellTune, kBellDecay, kBellChar};
const uint8_t kHats[] = {kHatClosed, kHatOpen, kHatTune, kHatClosedDecay, kHatOpenDecay, kHatTone, kHatChar};
const uint8_t kCym[] = {kCymLevel, kCymTune, kCymDecay, kCymTone, kCymChar};
const uint8_t kSendList[] = {48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69};
const uint8_t kGlobal[] = {kAccent, kDrive, kWidth};

const Section kSections[] = {
    OT_SYNTH_SECTION("Kick", "Kick", kKick),       OT_SYNTH_SECTION("Snare", "Snare", kSnare),
    OT_SYNTH_SECTION("Clap", "Clap", kClap),       OT_SYNTH_SECTION("Toms", "Toms", kToms),
    OT_SYNTH_SECTION("Perc", "Rim & Bell", kPerc), OT_SYNTH_SECTION("Hats", "Hi Hats", kHats),
    OT_SYNTH_SECTION("Cymbal", "Cymbal", kCym),    OT_SYNTH_SECTION("Sends", "Sends", kSendList),
    OT_SYNTH_SECTION("Global", "Global", kGlobal), OT_SYNTH_MACRO_SECTIONS};
static_assert(sizeof(kSendList) == 2 * kSounds, "one Delay and one Reverb send per sound");

}  // namespace

extern const SynthDescriptor kTtypeDescriptor;
const SynthDescriptor kTtypeDescriptor = {audio::ttype::kEngineId, "TTYPE", kSynthFlagKitState, -1,
                                         uint8_t(audio::ttype::kParamCount), kParams, kUi, kSections,
                                         uint8_t(sizeof(kSections) / sizeof(kSections[0])), 0, nullptr, "T-Type"};

}  // namespace mod
}  // namespace ot
