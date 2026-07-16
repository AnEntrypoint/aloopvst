// FULL dubfx effect chain — the complete ../looper audio effect path as a
// single Faust process, in the exact loopMachine::update order:
//   input -> PITCH (exact engine) -> DELAY+REVERB sends -> MICROREPEAT
//         -> HP filter -> LP filter -> output
//
// Each stage reuses the verified per-stage implementation via component().
// All knobs are per-render constants (piecewise-constant per block, as the
// looper drives them). This is the top-level chain the whole task delivers.

import("stdfaust.lib");

// ---- Per-render params (all effect knobs; one per line for the render tool) ----
SEMIS    = 0.0;   // pitch semitones
FORMANT  = 0.0;   // pitch formant depth
ENGAGED  = 0.0;   // pitch engaged
DELAYAMT = 0.0;   // delay amount
REVAMT   = 0.0;   // reverb amount
TIME     = 0.5;   // delay+reverb time
DIV      = 0;     // microrepeat divisor
MLB      = 0;     // masterLoopBlocks
HPCUT    = 0.0;   // HP cutoff
LPCUT    = 1.0;   // LP cutoff
LPRES    = 0.0;   // LP resonance

// ---- Stage components (each verified independently) ----
pitchStage = component("pitch.dsp")[ SEMIS=SEMIS; FORMANT=FORMANT; ENGAGED=ENGAGED; ];
delayStage = component("delay.dsp")[ DELAYAMT=DELAYAMT; TIME=TIME; ];
reverbStage= component("reverb.dsp")[ REVAMT=REVAMT; TIME=TIME; ];
microStage = component("microrepeat.dsp")[ DIV=DIV; MLB=MLB; ];
filterStage= component("filters.dsp")[ HPCUT=HPCUT; LPCUT=LPCUT; LPRES=LPRES; ];

// Sends = delay then reverb (processSends does delay wet mix THEN reverb add,
// both in one per-sample loop; delay's wet is added first, then reverb reads
// the delayed signal — reproduce by chaining delayStage : reverbStage since
// reverb's input is l AFTER the delay wet mix).
sends = delayStage : reverbStage;

// Full chain in loopMachine order.
process = pitchStage : sends : microStage : filterStage;
