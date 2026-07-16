// aloop runtime effects chain — the dubfx effect stages with the params exposed
// as UI controls (hslider/checkbox) instead of compile-time constants, so the
// remappable control map can set the knobs LIVE. The DSP is the same verified
// dubfx math (imported from effects/home/faust/); only the param SOURCE changes
// from a baked constant to a runtime UI zone. The zone labels match targetToZone
// in the native shell (HPCUT, LPCUT, LPRES, REVAMT, DELAYAMT, TIME, FORMANT, SEMIS).
import("stdfaust.lib");

// runtime param controls (labels the control map targets bind to)
HPCUT    = hslider("HPCUT",   0.0, 0.0, 1.0, 0.001);
LPCUT    = hslider("LPCUT",   1.0, 0.0, 1.0, 0.001);
LPRES    = hslider("LPRES",   0.0, 0.0, 1.0, 0.001);
REVAMT   = hslider("REVAMT",  0.0, 0.0, 1.0, 0.001);
DELAYAMT = hslider("DELAYAMT",0.0, 0.0, 1.0, 0.001);
TIME     = hslider("TIME",    0.5, 0.0, 1.0, 0.001);
FORMANT  = hslider("FORMANT", 0.0, -3.0, 3.0, 0.001);
SEMIS    = hslider("SEMIS",   0.0, -12.0, 12.0, 0.001);
ENGAGED  = checkbox("ENGAGED");
// Microrepeat (apc_grid.cpp notes 82-86 -> fx/microrepeat_div): DIV is the beat
// divisor {0=off,1,2,4,8,16} set live from the control map; MLB is the current
// loop's length in blocks (masterLoopBlocks), read from the same varispeed grid
// the looper uses for Link sync so a repeat slice stays musically aligned.
DIV      = nentry("DIV", 0, 0, 16, 1);
MLB      = nentry("MLB", 0, 0, 4096, 1);

// Reuse the verified dubfx stage components with these runtime params.
filterStage = component("effects/home/faust/filters.dsp")[ HPCUT=HPCUT; LPCUT=LPCUT; LPRES=LPRES; ];
delayStage  = component("effects/home/faust/delay.dsp")[ DELAYAMT=DELAYAMT; TIME=TIME; ];
reverbStage = component("effects/home/faust/reverb.dsp")[ REVAMT=REVAMT; TIME=TIME; ];
microStage  = component("effects/home/faust/microrepeat.dsp")[ DIV=DIV; MLB=MLB; ];
pitchStage  = component("effects/home/faust/pitch.dsp")[ SEMIS=SEMIS; FORMANT=FORMANT; ENGAGED=ENGAGED; ];

// Second output: microStage's OWN output (post-glitch, pre-filter), a native
// tap so audio_thread.cpp can fold the STUTTERED signal back into next
// block's input -- the same one-block-lag native-mix technique used for the
// SHIFT-fold (aloop.dsp), so glitch content becomes recordable into a new
// loop, matching ../looper's real design (loopMachine.cpp:806-833's "that
// stutter becomes BOTH the audible output and the record source").
process = pitchStage : delayStage : reverbStage : microStage <: (filterStage, _);
