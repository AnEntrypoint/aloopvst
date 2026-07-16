// TPT State-Variable Filters — exact port of apcEffectsProcessor.h svfHighpass
// + svfLowpass + cubic soft-clip (Cytomic/Ableton topology).
//
// C++ per-sample update (both filters share this recurrence):
//   v3 = in - ic2eq;  v1 = a1*ic1eq + a2*v3;  v2 = ic2eq + a2*ic1eq + a3*v3;
//   ic1eq = 2*v1 - ic1eq;  ic2eq = 2*v2 - ic2eq;
// The two integrator states (ic1eq, ic2eq) are fed back with one unit delay
// via Faust's `~` operator. The feedback block takes (ic1_prev, ic2_prev, x)
// and emits (ic1_new, ic2_new); we tap the states to compute the outputs.
//
// Params are compile-time constants (one render = one fixed knob position),
// matching the looper's per-block piecewise-constant param behaviour.

import("stdfaust.lib");

SR = 48000.0;
PI = 3.14159265;

// ================= HIGHPASS (Q=0.707 Butterworth, no resonance) =============
// Feedback state (ic1, ic2) via `~`: the block takes the two fed-back states
// plus x, and emits (ic1', ic2', y). `~ si.bus(2)` loops the first two outputs
// back to the first two inputs with one unit delay; the trailing (!,!,_) drops
// the state taps and keeps only y.
hpK = 1.41421356;
hpG(cutoff) = tan(PI * (20.0 * pow(1000.0, cutoff)) / SR);

svfHPf(cutoff, x) = (blk ~ si.bus(2) : (!, !, _))
with {
    g  = hpG(cutoff);
    k  = hpK;
    a1 = 1.0 / (1.0 + g*(g+k));
    a2 = g*a1;
    a3 = g*a2;
    blk(ic1, ic2) = ic1n, ic2n, y
    with {
        v3  = x - ic2;
        v1  = a1*ic1 + a2*v3;
        v2  = ic2 + a2*ic1 + a3*v3;
        ic1n = 2.0*v1 - ic1;
        ic2n = 2.0*v2 - ic2;
        y = x - k*v1 - v2;   // highpass output
    };
};

// ================= LOWPASS (resonant, freq clamp, soft-clip) =================
lpMaxFreq = SR * 0.45;
lpG(cutoff) = tan(PI * min(20.0 * pow(1000.0, cutoff), lpMaxFreq) / SR);
lpK(res)    = 1.0 / (0.5 + (res*res) * 24.5);

softClip(x) = select2(x > 1.0,
                select2(x < -1.0, x - (x*x*x)/3.0, -2.0/3.0),
                2.0/3.0);

svfLPf(cutoff, res, x) = (blk ~ si.bus(2) : (!, !, _)) : softClip
with {
    g  = lpG(cutoff);
    k  = lpK(res);
    a1 = 1.0 / (1.0 + g*(g+k));
    a2 = g*a1;
    a3 = g*a2;
    blk(ic1, ic2) = ic1n, ic2n, v2
    with {
        v3  = x - ic2;
        v1  = a1*ic1 + a2*v3;
        v2  = ic2 + a2*ic1 + a3*v3;
        ic1n = 2.0*v1 - ic1;
        ic2n = 2.0*v2 - ic2;
    };
};

// ================= Guarded stages (match C++ skip conditions) ================
HPCUT = 0.0;
LPCUT = 1.0;
LPRES = 0.0;

hpStage(x) = select2(HPCUT > 0.01, x, svfHPf(HPCUT, x));
lpStage(x) = select2(LPCUT < 0.99, x, svfLPf(LPCUT, LPRES, x));

// processFilters order: HP then LP.
process = hpStage : lpStage;
