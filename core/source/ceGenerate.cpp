// tracker-mini-ce: Generate + Mutate (see ceGenerate.h). Our own code, written for this
// fork after the user's earlier stock-firmware Generate (gn1).

#include "ceGenerate.h"

#include <string.h>

#include "Platform.h"
#include "TrackerImpl.h"
#include "ceModsExtra.h"
#include "mtStructs.h"
#include "scales.h"

namespace ceGen
{
namespace
{
using Step = Sequencer::strPattern::strTrack::strStep;

uint32_t rngState = 0x9E3779B9u;

void seed()
{
    rngState ^= TrackerImpl::getInstance()->getPlatform()->getTimer()->micros() * 2654435761u;
    if (rngState == 0) rngState = 0x9E3779B9u;
}

uint32_t rnd()
{
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rngState = x;
}

int rndInt(int n) // 0 .. n-1
{
    return n <= 1 ? 0 : (int)(rnd() % (uint32_t)n);
}

bool chance(int percent)
{
    return rndInt(100) < percent;
}

// the project's scale as a list of notes (up to 3 octaves from the base)
struct Scale
{
    int8_t notes[40];
    int count = 0;

    void build(int base, int octaves)
    {
        uint8_t root = mtProject.values.padBoardRootNote % 12;
        uint8_t scale = mtProject.values.padBoardScale;
        count = 0;
        for (int n = base; n < base + 12 * octaves && n <= 127 && count < 40; n++)
            if (isInScale((uint8_t)n, root, scale)) notes[count++] = (int8_t)n;
        if (count == 0) notes[count++] = (int8_t)base;
    }
    int8_t at(int degree) const
    {
        if (degree < 0) degree = 0;
        if (degree >= count) degree = count - 1;
        return notes[degree];
    }
    int degreeOf(int note) const
    {
        int best = 0;
        for (int i = 0; i < count; i++)
            if (abs(notes[i] - note) < abs(notes[best] - note)) best = i;
        return best;
    }
};

int baseNote(int octave) // the root note in the given octave (C3 = 48)
{
    return 12 * octave + (mtProject.values.padBoardRootNote % 12);
}

int density(int fallback)
{
    uint8_t d = ceExtra::at(ceExtra::kGenDensity);
    return d == 0 ? fallback : (d > 10 ? 100 : d * 10);
}

uint8_t trackInstrument(Sequencer::strPattern* p, uint8_t track)
{
    Sequencer::strPattern::strTrack& t = p->track[track];
    for (int s = 0; s <= p->track[0].length; s++)
        if (t.step[s].note >= 0) return t.step[s].instrument;
    return mtProject.values.lastUsedInstrument;
}

void clearStep(Step& st)
{
    st.note = Sequencer::STEP_NOTE_EMPTY;
    st.instrument = 0;
    for (int f = 0; f < Sequencer::kStepFxSlots; f++)
    {
        st.fx[f].type = 0;
        st.fx[f].value = 0;
    }
    st.volume = Sequencer::STEP_VELO_DEFAULT;
    st.micromove = -1;
}

void setNote(Step& st, int note, uint8_t instrument, int volume = -1)
{
    st.note = (int8_t)(note < 0 ? 0 : (note > 127 ? 127 : note));
    st.instrument = instrument;
    st.volume = (int8_t)volume;
}

void setFx(Step& st, uint8_t type, uint8_t value)
{
    for (int f = 0; f < Sequencer::kStepFxSlots; f++)
    {
        if (st.fx[f].type == 0 || st.fx[f].type == type)
        {
            st.fx[f].type = type;
            st.fx[f].value = value;
            return;
        }
    }
}

// Bjorklund: k hits spread over n steps
void euclid(bool* out, int k, int n)
{
    int bucket = 0;
    for (int i = 0; i < n; i++)
    {
        bucket += k;
        out[i] = false;
        if (bucket >= n)
        {
            bucket -= n;
            out[i] = true;
        }
    }
}
} // namespace

const char* name(uint8_t algorithm)
{
    static const char* const names[kAlgorithmCount] = { "Acid", "Euclid", "Hip-Hop",
                                                         "Break Cut", "Walk", "Arp" };
    return names[algorithm < kAlgorithmCount ? algorithm : 0];
}

void generate(Sequencer::strPattern* p, uint8_t track, uint8_t algorithm)
{
    seed();
    int len = p->track[0].length + 1;
    uint8_t instr = trackInstrument(p, track);
    Sequencer::strPattern::strTrack& t = p->track[track];
    for (int s = 0; s < len; s++) clearStep(t.step[s]);

    Scale sc;
    switch (algorithm)
    {
        case kAcid:
        {
            sc.build(baseNote(3), 2);
            int dens = density(70);
            int rootDeg = 0;
            for (int s = 0; s < len; s++)
            {
                if (!chance(dens) && s % 4 != 0) continue;
                int deg = chance(45) ? rootDeg : rndInt(sc.count / 2 + 1);
                int note = sc.at(deg);
                if (chance(18)) note += 12;   // octave jump
                bool accent = chance(28);
                setNote(t.step[s], note, instr, accent ? 127 : 88);
                if (chance(22) && s + 1 < len) setFx(t.step[s + 1], Sequencer::strFxConsts::enFxType::FX_TYPE_GLIDE, 30);
            }
            break;
        }
        case kEuclid:
        {
            bool hits[Sequencer::MAXSTEP + 1];
            int k = (len * density(35) + 50) / 100;
            if (k < 1) k = 1;
            euclid(hits, k, len);
            int rot = rndInt(len);
            int note = baseNote(4);
            bool first = true;
            for (int s = 0; s < len; s++)
            {
                if (!hits[(s + rot) % len]) continue;
                setNote(t.step[s], note, instr, first ? 127 : 100);
                first = false;
            }
            break;
        }
        case kHipHop:
        {
            // a kick-like part: the one, the "and" of two, pickups; late off-beats, ghosts
            int dens = density(50);
            int note = baseNote(4);
            for (int s = 0; s < len; s++)
            {
                int pos = s % 16;
                int p0 = 0;
                if (pos == 0) p0 = 100;
                else if (pos == 10) p0 = 85;
                else if (pos == 7 || pos == 3) p0 = 35;
                else if (pos == 14 || pos == 15) p0 = 30;
                else if (pos % 2 == 1) p0 = 12;
                p0 = p0 * dens / 50;
                if (!chance(p0)) continue;
                bool ghost = (pos % 4) != 0 && pos != 10 && chance(50);
                setNote(t.step[s], note, instr, ghost ? 55 : (pos == 0 ? 127 : 105));
                if (pos % 2 == 1) t.step[s].micromove = (int8_t)(15 + rndInt(20)); // laid back
            }
            break;
        }
        case kBreakCut:
        {
            // slices 0..15 of a sliced sample, played mostly in order
            int note = baseNote(4);
            int dens = density(85);
            int prev = 0;
            for (int s = 0; s < len; s++)
            {
                if (!chance(dens)) continue;
                int slice = s % 16;
                if (chance(25)) slice = rndInt(16);
                else if (chance(12)) slice = prev;
                prev = slice;
                setNote(t.step[s], note, instr, chance(20) ? 80 : -1);
                setFx(t.step[s], Sequencer::strFxConsts::enFxType::FX_TYPE_SAMPLE_SLICE, (uint8_t)slice);
                if (chance(6)) setFx(t.step[s], Sequencer::strFxConsts::enFxType::FX_TYPE_REVERSE_PLAYBACK, 1);
            }
            break;
        }
        case kWalk:
        {
            sc.build(baseNote(4), 2);
            int dens = density(60);
            int deg = sc.count / 4;
            for (int s = 0; s < len; s++)
            {
                if (!chance(dens) && s != 0) continue;
                int step = rndInt(5) - 2; // -2 .. +2
                if (step == 0 && chance(50)) step = 1;
                deg += step;
                if (deg < 0) deg = 1;
                if (deg >= sc.count) deg = sc.count - 2;
                setNote(t.step[s], sc.at(deg), instr, 80 + rndInt(40));
            }
            break;
        }
        case kArp:
        default:
        {
            sc.build(baseNote(4), 3);
            // the scale's triad (degrees 0, 2, 4) and octave, up, down or up-down
            int chord[4] = { 0, 2, 4, 7 };
            int dir = rndInt(3);
            int rate = density(100) >= 75 ? 1 : 2;
            int idx = 0, d = 1;
            for (int s = 0; s < len; s += rate)
            {
                int c = idx;
                if (dir == 1) c = 3 - idx;
                setNote(t.step[s], sc.at(chord[c]), instr, (s % 4 == 0) ? 120 : 92);
                if (dir == 2)
                {
                    idx += d;
                    if (idx >= 3) d = -1;
                    if (idx <= 0) d = 1;
                }
                else
                    idx = (idx + 1) % 4;
            }
            break;
        }
    }
}

void mutate(Sequencer::strPattern* p, uint8_t track)
{
    seed();
    uint8_t m = ceExtra::at(ceExtra::kGenMutate);
    int amount = m == 0 ? 25 : (m > 10 ? 100 : m * 10);
    int len = p->track[0].length + 1;
    Sequencer::strPattern::strTrack& t = p->track[track];
    uint8_t instr = trackInstrument(p, track);

    // notes already in the track (to borrow from), and the scale around them
    int notes[Sequencer::MAXSTEP + 1];
    int noteCount = 0;
    for (int s = 0; s < len; s++)
        if (t.step[s].note >= 0) notes[noteCount++] = t.step[s].note;
    if (noteCount == 0) return; // nothing to mutate

    Scale sc;
    sc.build(notes[0] >= 24 ? notes[0] - 24 : 0, 5);

    for (int s = 0; s < len; s++)
    {
        Step& st = t.step[s];
        if (!chance(amount)) continue;
        if (st.note >= 0)
        {
            int r = rndInt(100);
            if (r < 40)
            {
                int deg = sc.degreeOf(st.note) + (chance(50) ? 1 : -1);
                st.note = sc.at(deg);
            }
            else if (r < 55)
            {
                clearStep(st);
            }
            else if (r < 80)
            {
                int v = st.volume < 0 ? 100 : st.volume;
                v += rndInt(41) - 20;
                st.volume = (int8_t)(v < 20 ? 20 : (v > 127 ? 127 : v));
            }
            else
            {
                int to = s + (chance(50) ? 1 : -1);
                if (to >= 0 && to < len && t.step[to].note < 0 &&
                    t.step[to].note != Sequencer::STEP_NOTE_OFF_CUT &&
                    t.step[to].note != Sequencer::STEP_NOTE_OFF_FADE &&
                    t.step[to].note != Sequencer::STEP_NOTE_OFF_DEFAULT)
                {
                    t.step[to] = st;
                    clearStep(st);
                }
            }
        }
        else if (st.note == Sequencer::STEP_NOTE_EMPTY && chance(30))
        {
            setNote(st, notes[rndInt(noteCount)], instr, 70 + rndInt(50));
        }
    }
}
} // namespace ceGen
