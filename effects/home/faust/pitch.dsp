// Live pitch shifter — the EXACT ../looper EngineSoladSnac, linked via ffunction
// (dsp/pitch_ffi.h wrapping soladSnacOctaver.h). This is not an approximation:
// the pitched sample is produced by the real C++ engine, so the stage is
// bit-identical to the looper by construction.
//
// Params ride WITH the sample into dubfx_pitch_tick (a separate param-only call
// gets constant-folded away by Faust), which applies them to the engine each
// sample (idempotent) then processes.

import("stdfaust.lib");

pitchTick = ffunction(float dubfx_pitch_tick(float, float, float, float), "pitch_ffi.h", "");

// Per-render params.
SEMIS   = 0.0;    // semitones (0 = unity)
FORMANT = 0.0;    // formant depth [-1,+1]
ENGAGED = 0.0;    // 0 = disengaged (bypass), 1 = engaged

scale = pow(2.0, SEMIS / 12.0);

process = _, scale, FORMANT, ENGAGED : pitchTick;
