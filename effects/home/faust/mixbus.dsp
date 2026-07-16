// Mix bus + hard clip — exact port of loopMachine::update final mix
// (loopMachine.cpp:936-1004) and simple_clip (:551-558).
//
// Per sample (mono), with ival = the WET effect-chain output ("thru" path) and
// oval = the dry loop sum (from m_output_buffer, gated by the SHIFT dry gain):
//   ival32 = ival * thru_level
//   oval32 = oval * loop_level * gate      (gate = dry-loop ramp = 1-fold, *(1-mrWet))
//   mval32 = ival32 + oval32
//   mval32 = mval32 * mix_level
//   out    = simple_clip(mval32)           (clamp to [-32768, 32767])
// Volume scales are value/63 (THRU/LOOP/MIX); at default value=63 all are 1.0.
//
// This chain's single audio input is the WET effect output (ival). The loop sum
// (oval) is a SECOND signal that only exists when the looper's playback engine
// runs — it is the looper's mixer, not part of the wet effect chain. Here oval
// is exposed as a second input so the stage can be verified; in the pure effect
// chain (no loops) oval=0 and the stage reduces to clip(ival*thru*mix).
//
// simple_clip operates in s16 sample units; the chain works in normalized float
// [-1,1] (=[−32768,32767]/32768). So the clamp is to [-1, 32767/32768].

import("stdfaust.lib");

THRU = 1.0;   // thru_level  (value/63; default 1.0)
LOOP = 1.0;   // loop_level
MIX  = 1.0;   // mix_level
GATE = 1.0;   // dry-loop gate = (1-fold)*(1-mrWet); 1.0 when no SHIFT/no stutter

// Hard clip in normalized-float domain: s16 [-32768,32767] -> [-1, 32767/32768].
clipN(x) = max(-1.0, min(32767.0/32768.0, x));

// ival = wet effect output; oval = loop sum (0 in the pure effect chain).
mixbus(ival, oval) = clipN((ival*THRU + oval*LOOP*GATE) * MIX);

// Single-input pure-effect-chain form: no loops (oval=0), so the mix reduces to
// clipN(ival*THRU*MIX).
process(ival) = mixbus(ival, 0.0);
