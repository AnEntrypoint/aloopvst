// aloop home stack — the loop engine + the dubfx effect chain as ONE Faust
// program (both are Faust, so they compose directly). This is the fixed home
// stack that runs on Core 1; the user's swappable effect is a separate LV2 on
// Core 3 (loaded by the in-process host). See docs/ARCHITECTURE.md.
//
// Signal: input -> loop.dsp (20 independent record/play loopers, no overdub) -> the dubfx
// effect chain (pitch/delay/reverb/microrepeat/filters) -> output.
//
// SHIFT-held monitor-fold moved OUT of this Faust graph and into
// audio_thread.cpp's worker() (native, block-rate): folding loopSum into `fx`'s
// input HERE (same-block, live-only) could never be recorded, since loop.dsp's
// record path runs BEFORE this fold -- a Faust `~`-recursion fix for that was
// tried and WITNESSED to silently break basic dry passthrough live on real
// hardware (git history: 90083c4 -> 3b8bd5e revert). The native fix instead
// feeds the PREVIOUS block's RAW loop-engine output (this program's second
// output, below) back into `fin` (the engine's own input) before
// faustHome.compute() runs, so THIS Faust graph stays exactly as
// simple/proven as before -- loop.dsp's record path sees the fold because
// it's baked into the input signal itself, one block later, matching
// looper's real one-block-lag fold (loopMachine.cpp:709-741) exactly.
//
// BUG FOUND AND FIXED (unconditional loop-into-record leak, WITNESSED via
// hardware telemetry + code trace): this file used to ALSO gate a direct
// `loopSum*(1.0-monitorFold)` term into fxOuts below, controlled by a Faust
// `MONITORFOLD` hslider zone. That zone is DEAD -- audio_thread.cpp stopped
// pushing "fx/monitorfold" into any Faust zone once the native fold above
// was introduced (it reads the ParamStore value directly instead; see its
// own top-of-worker comment) -- so `monitorFold` was permanently stuck at
// its compiled-in default 0.0, meaning `(1.0-monitorFold)` was ALWAYS 1.0
// and `loopSum` (full, ungated, every block) was unconditionally summed
// into `fxOuts` regardless of SHIFT state. `rawGlitchTap` (derived from
// fxOuts) is exactly what audio_thread.cpp feeds back as `glitchIn` into
// EVERY looper's record path every block -- so this dead gate was the
// actual mechanism silently making all loop content recordable into any
// armed looper at all times, not just during a genuine SHIFT hold.
// Fixed by dropping the term entirely: `dry` (=`fin`) already carries the
// SHIFT-gated loop-fold via the native prevLoopSum/foldGain mechanism one
// block later, so re-adding raw `loopSum` here was both redundant for the
// audible path and, worse, the actual leak for the record path. `fxOuts`
// is now just `dry : fx`, matching what the native fold already provides.
//
// Composing this way means the ENTIRE home audio path is one Faust compile — the
// maintainability win: change a knob mapping or a stage in Faust, rebuild, done.
// No hand-written C++ DSP anywhere in the home stack.
//
// GLITCH (microrepeat) RECORDABILITY: a prior attempt fed microStage's own
// post-glitch tap (rawGlitchTap, below) back into `fin`/`in` next block, same
// as the SHIFT-fold above -- WITNESSED to create a genuine one-block feedback
// whine, because `in` flows through `fx` (hence microStage) AGAIN every block,
// compounding. Fixed by giving `loop` a SECOND, dedicated, record-only input
// (glitchIn) that only the record-capture term consumes (dsp/loop.dsp's
// oneLooper) -- it never touches the dry/live path, so it cannot re-enter
// `fx`/microStage on any later block. See audio_thread.cpp's prevGlitchTap.
//
// GLITCH-HELD LOOP-ROUTING (GLITCHFOLD, distinct from the above): the above
// paragraph is about microStage's OWN output being recordable; this is about
// the LOOPERS' output being routed INTO microStage while glitch is held, so
// glitch "replaces" normal loop playback rather than playing alongside it
// (user-confirmed requirement). This reuses the EXACT SAME native mechanism
// as the SHIFT-fold (audio_thread.cpp's prevLoopSum -- the raw LOOP ENGINE
// output, NOT rawGlitchTap/microStage's output, so this is not the whine
// path described above), just gated by glitch-engaged instead of
// SHIFT-held, with its own GLITCHFOLD zone complementarily suppressing the
// direct raw-loopSum term below (mirrors MONITORFOLD). See
// audio_thread.cpp's "GLITCH-HELD loop-routing fold" comment for the native
// side and the SHIFT+glitch-simultaneous combine-and-clamp reasoning.

import("stdfaust.lib");

// The loop engine (20 independent record/play loopers, no overdub).
loop = component("loop.dsp");
// The RUNTIME effects chain — the verified dubfx stages, but with the params as
// live UI controls (dsp/effects_runtime.dsp) so the remappable control map can
// set the knobs at runtime (the dubfx chain.dsp bakes them as constants; that
// stays the A/B reference, this is the runtime variant).
fx   = component("effects_runtime.dsp");

// MONITORFOLD Faust zone REMOVED (see top-of-file BUG FOUND AND FIXED
// comment): it was dead (nothing writes to it anymore -- the SHIFT-fold
// gating lives entirely in audio_thread.cpp's native prevLoopSum/foldGain
// mix now), and its stuck-at-0.0 default was the actual mechanism letting
// loop content leak into every looper's record path at all times via
// rawGlitchTap/glitchIn, regardless of SHIFT state.
//
// REGRESSION FOUND AND FIXED (WITNESSED live: "no loops play back" after
// the fix above): dropping the dead MONITORFOLD term ALSO dropped `loopSum`
// from `fxOuts` entirely -- not just from the record-leak path. That threw
// away NORMAL loop playback (always-on, independent of SHIFT) from the live
// audible mix, since `dry` alone is just the raw external input with no
// loop content at all. The actual fix needed is narrower: `loopSum` must
// stay summed into the audible mix unconditionally (that's just playback),
// while ONLY `rawGlitchTap` (which feeds every looper's record-only
// glitchIn path) must exclude the SHIFT-gated portion so loop content isn't
// unconditionally recordable. Since the native prevLoopSum/foldGain
// mechanism in audio_thread.cpp already re-adds SHIFT-held loop content
// into `dry` one block later (so it reaches BOTH the live mix and the
// record path together, correctly gated), summing raw `loopSum` here too
// would double it during a SHIFT hold -- so `loopSum` is summed into the
// audible path directly (normal always-on playback) while `fx`'s glitch tap
// (and therefore glitchIn) is derived from `dry` alone, exactly as before
// this file's SHIFT-fix, keeping loop content OUT of automatic
// recordability except via the native one-block-lag SHIFT-fold path.

// MONITORFOLD is back as a live Faust zone -- audio_thread.cpp now writes
// its OWN native foldGain ramp into this zone every block (see its
// "SHIFT-held monitor-fold" comment), so it genuinely reflects SHIFT state
// this time (unlike the removed one, which nothing ever wrote to). Used
// ONLY to complementarily gate the DIRECT raw-`loopSum` term below -- not to
// reintroduce loopSum into `fxOuts`/the glitch tap, which is what caused
// the earlier record-leak bug. si.smoo matches looper's MONITOR_GATE_STEP.
monitorFold = hslider("MONITORFOLD", 0.0, 0.0, 1.0, 1.0) : si.smoo;

// GLITCHFOLD: same pattern as MONITORFOLD above, but reflecting glitch
// (microrepeat) engagement instead of SHIFT-held. audio_thread.cpp writes
// its OWN native glitchFoldGain ramp into this zone every block (gated by
// fx/microrepeat_div > 0), mirroring foldGain's SHIFT-held ramp exactly --
// see its "GLITCH-HELD loop-routing fold" comment. User-confirmed
// requirement: "glitch should replace normal loop output while held" --
// the direct raw-loopSum term below must fade OUT while glitch is engaged,
// exactly as it already fades out while SHIFT is held, so loop content
// becomes audible ONLY via the fx-routed (glitched) path in that case, not
// doubled/layered with the unprocessed direct term.
glitchFold = hslider("GLITCHFOLD", 0.0, 0.0, 1.0, 1.0) : si.smoo;

// `loop`'s two outputs (dry, loopSum) are consumed by `mixAndFx`:
//   - `fxOuts = dry : fx` -- fx's input (and therefore the glitch tap fed
//     back as glitchIn) is `dry` ALONE, so loop content is never
//     automatically recordable except via the native SHIFT-fold's or
//     GLITCH-fold's one-block-lag re-entry into `dry`
//     (audio_thread.cpp's prevLoopSum, combined-and-clamped there).
//   - `filtOut` sums fxOuts' filtered output with `loopSum` GATED by
//     `(1-monitorFold)*(1-glitchFold)` -- normal playback (neither SHIFT
//     nor glitch held) hears loops directly/unprocessed by fx, exactly
//     matching looper's real monitor path; while EITHER SHIFT or glitch is
//     held, this direct raw term fades OUT (its own gate -> 1) as the
//     matching native one-block-lag fold fades loop content IN through
//     `dry`/`fx` instead (audio_thread.cpp's foldGain/glitchFoldGain
//     ramps), so the audible signal crossfades from raw to fx-processed
//     loop content rather than ever summing both at once -- REGRESSION
//     FOUND AND FIXED (SHIFT case): WITNESSED live as "no loops play back"
//     when a prior fix removed loopSum from the mix entirely instead of
//     just re-gating it; the glitch case reuses the identical gating shape
//     to avoid repeating that mistake.
//     Four total program outputs, in this order:
//   1. finalOut  -- the real audible/wire signal (fouts[0] in audio_thread.cpp)
//   2. rawGlitchTap -- microStage's own post-glitch signal (fouts[1]); kept
//      as a structural output but no longer consumed for record-folding
//      (see RECORD-ALWAYS-EFFECTED below -- recordTap/prevFiltIn superseded
//      the old glitchIn-only mechanism this tap used to feed).
//   3. rawLoopSum -- the loop engine's own output (fouts[2]), so the native
//      SHIFT-fold mix (see top-of-file comment) can fold it into next
//      block's input, matching looper's m_input_buffer += m_output_buffer*fg
//      (loopMachine.cpp:738) -- the RAW loop output, not the fully-effected
//      wet signal (which would compound effects every block the fold is held).
//   4. recordTap -- the fx-EFFECTED signal alone (fouts[3], NOT filtOut --
//      see the REGRESSION FOUND AND FIXED note below for why they must
//      differ), so audio_thread.cpp can snapshot it into prevFiltOut for
//      next block's DEDICATED record-only input (see RECORD-ALWAYS-EFFECTED
//      below and loop.dsp's oneLooper) without ever including the
//      unconditional direct-playback term.
// directFoldSuppress: the direct raw-loopSum term is suppressed whenever
// EITHER SHIFT (monitorFold) OR glitch (glitchFold) is fading its own
// fx-routed copy of loop content IN via `dry` (audio_thread.cpp's native
// one-block-lag fold, combined-and-clamped there so a simultaneous SHIFT+
// glitch hold folds loop content in at most once, not twice). Multiplying
// the two complements (rather than summing the raw folds here) means either
// control alone fully suppresses the direct term at its own gain, and both
// held together still bottoms out at (1-1)*(1-1)=0 -- never negative, never
// double-counted -- matching the native side's single clamped fin[] fold.
// RECORD-ALWAYS-EFFECTED (4th output, filtOut tapped again as recordTap):
// user-confirmed requirement -- recording must ALWAYS capture the fully
// effected signal (through pitch/delay/reverb/microrepeat/filters), not the
// raw pre-fx input, unconditionally (not just under SHIFT). Cannot wire
// filtOut directly into loop.dsp's record path THIS block (loop's record
// term runs BEFORE fx even executes -- `process(in,...) = loop(in,...) :
// mixAndFx`, loop first) and cannot feed it back into `in`/dry either (would
// re-enter `fx` AGAIN next block, the exact whine bug class dafa945 fixed
// for glitch specifically, now an even worse surface covering every stage).
// So this reuses the IDENTICAL native one-block-lag technique as
// prevGlitchTap/prevLoopSum: filtOut is tapped a SECOND time here as a
// dedicated 4th process() output (recordTap, numerically identical to
// output 1, just duplicated so audio_thread.cpp can snapshot it into
// prevFiltOut without touching the live audible path), fed back next block
// as loop.dsp's new record-only input (see loop.dsp's oneLooper: record =
// prevFiltIn * recN). Since SHIFT-fold and GLITCH-fold both already put loop
// content into `dry` BEFORE this block's `fx` call (audio_thread.cpp's
// prevLoopSum fold into fin), that folded content is ALREADY part of THIS
// block's filtOut/recordTap -- so a SHIFT-held or glitch-held recording
// captures effected loop content automatically via this SAME single tap,
// no separate term needed.
// REGRESSION FOUND AND FIXED (WITNESSED live: "loops are now incorrectly
// recording without shift being held"): recordTap was set to `filtOut`,
// but `filtOut` unconditionally includes `loopSum*directFoldSuppress` --
// the ALWAYS-ON direct raw-playback term for normal (non-SHIFT, non-glitch)
// loop audibility. Since recordTap becomes every looper's record-only input
// one block later, that meant ALL currently-playing loop content was
// automatically recordable at all times again, exactly the bug 7aec025
// fixed for the OLD glitch-only tap, now reintroduced for the whole fx
// chain by RECORD-ALWAYS-EFFECTED's redesign. Fixed by deriving recordTap
// from `fxEffected` (fxOuts' filtered output ALONE, no direct loopSum term)
// instead of `filtOut` -- fxEffected already contains: live input (always
// effected, per the user's requirement), PLUS SHIFT-held loop content
// (via the native prevLoopSum fold into `dry`), PLUS glitch-held loop
// content (same fold, glitch-gated) -- everything that should be
// recordable -- while excluding the unconditional direct-playback term
// that must stay audible-only, never automatically recordable.
mixAndFx(dry, loopSum) = filtOut, rawGlitchTap, loopSum, recordTap
with {
    fxOuts = dry : fx;
    directFoldSuppress = (1.0-monitorFold) * (1.0-glitchFold);
    fxEffected = fxOuts : (_, !);
    filtOut = fxEffected + loopSum*directFoldSuppress;
    rawGlitchTap = fxOuts : (!, _);
    recordTap = fxEffected;
};
// prevFiltIn: previous block's fully-effected mix output (audio_thread.cpp's
// prevFiltOut), fed ONLY into loop's dedicated record-only input (see
// loop.dsp's oneLooper comment) -- never mixed into `in`/dry, so it cannot
// re-enter `fx` on this or any later block. This REPLACES the old glitchIn
// input: prevFiltIn already contains post-glitch content one block later
// (effects_runtime.dsp's stage order has microStage upstream of
// filterStage, so microStage's output is already baked into filtOut),
// making a separate glitch-only term redundant/double-counting. Second
// process() input.
//
// clearAll/effSpeed: 3rd and 4th process() inputs, the momentary global
// commands (hardware CLEAR_ALL, HALFSPEED/DOUBLESPEED) and now TRUE
// varispeed's combined playback-rate multiplier (manual speed x Link-tempo
// ratio, see loop.dsp's top-of-file comment). Previously declared as Faust
// UI zones (button("clear")/hslider("speed", ...)) inside loop.dsp, which
// par()'s per-looper replication silently duplicated into 20 separate zones
// even after being "hoisted" outside the vgroup textually -- see loop.dsp's
// ROOT CAUSE comment above oneLooper for the full trace (Faust's par() is a
// code-generating combinator: an expression passed as an argument into a
// replicated block gets re-elaborated, UI declaration included, at each of
// the 20 call sites). Routed here as plain signal inputs instead, the same
// technique as prevFiltIn -- audio_thread.cpp pushes the computed
// clear/effSpeed values directly into fins[2]/fins[3] every block (constant
// across the block, no interpolation needed since they are momentary step/
// slow-changing values, not per-sample-varying control), and they reach
// every looper identically via loop.dsp's par() without ever being
// independently instantiated.
process(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen) = loop(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen) : mixAndFx;
