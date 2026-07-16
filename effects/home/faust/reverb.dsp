// Reverb — exact port of apcEffectsProcessor::processSends (reverb portion).
// Freeverb-style: 8 parallel comb lines with one-pole lowpass damping.
//
// C++ per line (fixed integer length L; no fractional read -> far more exact
// than the tape delay):
//   lineOut  = ring[pos]
//   filter   = filter*damp + lineOut*(1-damp)      // one-pole damping (state)
//   dampedOut = filter
//   input    = (line<4 ? l : r) * 0.15             // mono: l==r
//   fed      = input + dampedOut*decay             // clamp +-4.0
//   ring[pos] = fed
//   pos      = (pos+1) % L
//   accumulate dampedOut into revL (lines 0-3) / revR (lines 4-7)
// Then: out += rev * amount * 0.25 (per side; mono sums both halves' scale).
//
// decay = 0.70 + time*0.25 + amount^2*0.05, clamped <= 0.998
// damp  = 0.7 - amount*0.4, clamped >= 0.3
// Guard: reverb only runs when amount > 0.001 (else pure passthrough).

import("stdfaust.lib");

SR   = 48000.0;

REVAMT = 0.0;    // per-render constant
TIME   = 0.5;

lens = (2473, 2767, 3217, 3571, 3907, 4057, 2143, 1933);
lenAt(i) = ba.take(i + 1, lens);

decayC(amt, t) = min(0.998, 0.70 + t*0.25 + amt*amt*0.05);
dampC(amt)     = max(0.3, 0.7 - amt*0.4);

clip4(x) = max(-4.0, min(4.0, x));

// One comb line, length L. C++ per-sample order:
//   lineOut  = ring[pos]                          (== fed written L samples ago)
//   fmem     = fmem*damp + lineOut*(1-damp)       (damping one-pole, updated)
//   dampedOut= fmem                               (POST-update)
//   fed      = clip4(input + dampedOut*decay)
//   ring[pos]= fed ; pos = (pos+1) % L
// So lineOut[n] = fed[n-L], and dampedOut uses the just-updated fmem.
//
// Faust letrec: 'fmem is the recursive damping state (1-sample delay); lineOut
// is fed delayed by exactly L. `fed` is combinational within the group (it is
// the ring's write value; its delayed tap is lineOut). dampedOut = fmem here
// refers to the CURRENT (updated) value because in a letrec the unprimed name
// is the current-sample signal and 'fmem is its next value... but the C++
// dampedOut is the POST-update filter, i.e. fmem AFTER this sample's update.
// We therefore compute the updated filter explicitly as `fnew` and use it for
// both dampedOut and the fed-back state.
// Damping one-pole: fmem[n] = fmem[n-1]*damp + in[n]*(1-damp).
//   in*(1-damp) : (+ ~ *(damp))   ==   y[n] = in[n]*(1-damp) + y[n-1]*damp.
dampFilter(damp) = *(1.0 - damp) : (+ ~ *(damp));

// Comb line: single `~` feedback over the ring signal `fed`.
//   dampedOut = dampFilter( fed delayed by L )     (POST-update filter)
//   fed       = clip4( input + dampedOut*decay )
// The de.delay(L) breaks the loop, so the `~` is well-formed. No inline lambdas
// (Faust anonymous-fn syntax is fragile in feedback position) — a named 1-arg
// generator `combGen` is used with `~ _`.
combLineExact(L, decay, damp, input) = dampedOut
with {
    tap(f)    = f : de.delay(8192, L) : dampFilter(damp);   // fed@L -> damping
    combGen(f) = clip4(input + tap(f)*decay);               // ring write value
    fed       = combGen ~ _;
    dampedOut = tap(fed);
};

// MONO reality: loopMachine feeds the same mono buffer as both l and r to
// processSends, and reads back ONLY channel 0 (left). The C++ splits lines 0-3
// into revL (added to l) and 4-7 into revR (added to r); since only l is kept,
// ONLY LINES 0-3 contribute to the mono output. Lines 4-7 are computed and
// discarded. So the Faust mono reverb sums lines 0-3 only.
//   out = l + revL*amount*0.25,  revL = sum(dampedOut, lines 0..3)
reverb(amt, t, x) = x + revL * amt * 0.25
with {
    decay = decayC(amt, t);
    damp  = dampC(amt);
    input = x * 0.15;
    line(i) = combLineExact(lenAt(i), decay, damp, input);
    revL    = line(0) + line(1) + line(2) + line(3);   // lines 0-3 only (channel 0)
};

// Guard: amount <= 0.001 -> passthrough (matches C++ skip).
process = _ <: select2(REVAMT > 0.001, _, reverb(REVAMT, TIME));
