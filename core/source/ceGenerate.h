#ifndef CE_GENERATE_H_
#define CE_GENERATE_H_

// tracker-mini-ce: Generate + Mutate. Writes a new part into one track of the pattern,
// or changes some of the track's steps, in the project's scale and root note
// (Config -> Project Settings -> Notes scale / Root note). Pattern screen, More, More,
// button 6: tap = Generate, Shift + tap = Mutate, hold + turn = the algorithm.

#include "mtSequencer.h"

namespace ceGen
{
enum Algorithm : uint8_t
{
    kAcid,     // 303-style bass: accents, slides, octave jumps
    kEuclid,   // evenly spread hits (Bjorklund), rotated
    kHipHop,   // a laid-back groove: late off-beats, ghost notes
    kBreakCut, // chops a sliced sample: mostly in order, with jumps and repeats
    kWalk,     // a melody stepping through the scale
    kArp,      // an arpeggio over the scale's chord
    kAlgorithmCount
};

const char* name(uint8_t algorithm);

// the whole track (its pattern length); one undo step is stored by the caller
void generate(Sequencer::strPattern* pattern, uint8_t track, uint8_t algorithm);
void mutate(Sequencer::strPattern* pattern, uint8_t track);
} // namespace ceGen

#endif
