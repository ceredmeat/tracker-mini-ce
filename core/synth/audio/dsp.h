// tracker-mini-ce: OpenTracker compatibility layer (see synth_engine.h).
#pragma once
#include "audio/synth_engine.h"
#include <cmath>
#include <cstring>

namespace ot {
namespace audio {
inline void zeroWords(void* p, size_t bytes) { std::memset(p, 0, bytes); }
} // namespace audio
} // namespace ot
