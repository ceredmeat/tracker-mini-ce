# msfa (Music Synthesizer for All), DX7 core

From https://github.com/google/music-synthesizer-for-android (`app/src/main/jni`),
Copyright 2012 Google Inc., Apache License 2.0 (LICENSE). Used by tracker-mini-ce's DX7
engine (`core/synth/engines/Dx7SynthVoice.cpp`).

Changes made for tracker-mini-ce:
- `.cc` files renamed `.cpp` (the build picks up `.cpp`).
- `exp2.cpp`, `sin.cpp`, `freqlut.cpp`: the lookup tables placed in the external RAM on
  i.MX builds (`MSFA_TABLE`).
- `fm_core.cpp`: `<iostream>` and `FmCore::dump()` only with `VERBOSE`.
- `env.cpp`, `fm_core.cpp`, `dx7note.cpp`: `using namespace std;` removed (it made `min` /
  `max` ambiguous with `synth.h`'s).
- `aligned_buf.h`: includes `<stddef.h>` and `<stdint.h>`.
