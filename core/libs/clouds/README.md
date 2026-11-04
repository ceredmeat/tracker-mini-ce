# Clouds (Mutable Instruments), for the Looper

From https://github.com/pichenettes/eurorack (`clouds/`, master) and its `stmlib`
submodule at commit e3bd7c9 (in `../stmlib/`). MIT licence, Copyright 2014 Emilie Gillet:
see `LICENSE` here and `../stmlib/LICENSE`.

Used by the Looper's **Clouds** send effect (`../ceLooper/lp_fx.cpp`) and, for the tracks'
Granular playback mode, its grain scheduling, window and level normalisation
(re-implemented in `../ceLooper/lp_engine.cpp`, reading the loop instead of a record
buffer, with `lut_window` from `resources.cpp`).

Changes from the original:
- `.cc` files renamed `.cpp` (the firmware's makefile builds `.cpp`).
- `drivers/debug_pin.h`: the module's STM32 debug pin replaced by empty TIC / TOC.
- Only the files the granular processor needs are included.
- `dsp/mu_law.*`: `lut_ulaw` made `const`, so it stays in flash instead of on-chip RAM.

How it runs here (differences from the module, all outside these files):
- At 44.1 kHz instead of 32 kHz: grain sizes and times are about 30 % shorter in seconds,
  pitch is unchanged.
- Bigger buffers: 128 KB per channel (stereo 1.5 s, 8-bit stereo 3 s, mono 2 s at
  44.1 kHz; the module has 1 / 2 / 2 s). Kept under 128 K samples per channel because the
  grains use 16.16 fixed-point positions.
- Knob values stop just under 1.0 (a few table lookups read one entry past the end at
  exactly 1.0; harmless on the module, where that entry is multiplied by zero). One such
  read remains inside `Grain::RenderEnvelope` (the window at its peak); it is equally
  harmless and the original code is left as it is.
- `Prepare()` runs in the firmware's main loop (`AudioLooper::mainLoop()`), `Process()` in
  the audio interrupt, as on the module.
