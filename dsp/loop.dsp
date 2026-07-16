// aloop loop engine — in Faust. Matches the real hardware setup:
//   * 20 INDEPENDENT loopers (each its own record buffer + play head),
//   * record / play only — NO OVERDUB (the hardware has no overdub control),
//   * each looper's play RATE tracks manual half/double-speed AND the Ableton
//     Link grid (TRUE varispeed: tape-style pitch+duration change together),
//   * the 20 looper outputs sum to the engine output.
//
// TRUE VARISPEED (this generation): the WRITE side is still a simple
// monotonic-index ring write (record replaces the loop, no overdub, no
// read-modify-write of the SAME cell -- this is why the design compiles).
// The READ side is now a genuinely SEPARATE fractional accumulator
// (`readPos ~ _`, ordinary Faust signal recursion) advancing by `effSpeed`
// SAMPLES PER SAMPLE (not per block), wrapping at the loop's own length,
// driving TWO `rwtable` reads (floor/ceil index) linearly blended by the
// fractional part. This is NOT a read-modify-write: the read never writes
// back into the table at (or near) the position it read -- it only reads,
// exactly like looking up two arbitrary indices in an array -- so it does
// not hit the RMW rejection that blocked the EARLIER (different, now
// abandoned) attempt at a preserve-on-hold ADDRESSABLE playhead (which
// needed to write the read-back value into the SAME cell, a genuine RMW
// Faust's evaluator rejects, "endless evaluation cycle"/"stack overflow in
// eval", witnessed across 4 CI codegen attempts, see docs/DECISIONS.md ADR +
// .wfgy/lessons.md). Mark-point restart (SET/CLEAR_LOOP_START) and immediate
// re-trigger (LOOP_IMMEDIATE) still are NOT wired (a deliberate model
// difference documented in docs/COMMAND-SURFACE.md, unrelated to varispeed).
//
// Ported from looper's real mechanism (C:\dev\looper\loopClipUpdate.cpp's
// loopClip::update(), m_playPos/m_playRate; Looper.h's setMasterBlocks();
// loopMachine.cpp:637-663): `effectiveRate = m_playRate * g_globalSpeedMul`,
// where m_playRate = m_nativeBlocks/currentMasterBlocks (recomputed
// ABSOLUTELY, never accumulated, from the loop's ORIGINAL recorded length
// vs whatever length Link's current tempo implies) and g_globalSpeedMul is
// PURELY the manual half/double-speed button state (1.0/0.5/2.0), entirely
// separate from Link. aloop's native shell (audio_thread.cpp) computes this
// SAME product every block and pushes it in as `effSpeed`, a single
// process()-level SIGNAL INPUT (see the ROOT CAUSE comment below for why
// this must never become a UI-declared control).

import("stdfaust.lib");

SR       = 48000.0;
MAXLEN   = 48000 * 60;    // 60 s max loop per looper
NLOOPERS = 20;            // 20 independent loopers (the hardware setup)

// ---- one independent looper ----
// Controls (native shell drives these from MIDI + Link), indexed per looper i:
//   rec[i]   : 1 while recording looper i (replaces its loop; NO overdub)
//   play[i]  : 1 = looper i playing
//   len[i]   : looper i loop length in samples (from Link tempo for varispeed)
//   vol[i]   : looper i output level
// A looper is a one-loop-length feedback delay: while recording, write the live
// input; else recirculate (hold) the loop. There is no overdub path.
// One looper. The control labels use Faust's "[N]" group-index substitution so
// each of the 20 instances gets its own rec/play/len/vol control ("looper0/rec"
// … "looper19/rec"). Record replaces the loop; else it holds — NO overdub.
// Global controls (shared across all loopers): clear wipes every loop; speedMul
// scales the effective loop length for the momentary half/double-speed commands.
//
// ROOT CAUSE (2nd generation, THE REAL ONE): 382e775 hoisted clearAllGlobal/
// speedMulGlobal's button()/hslider() DECLARATIONS to file scope, outside the
// par(i, NLOOPERS, vgroup(...)) textually, and threaded them into oneLooper
// as ordinary function parameters -- but this did NOT stop Faust's compiler
// from still emitting 20 separate UI zones. WITNESSED via inspecting the
// generated C++ (build/loop.cpp): `grep -c '"speed"'` and `grep -c '"clear"'`
// each returned 20, and every occurrence is INSIDE its own "looper N" vgroup
// (`openVerticalBox("looper 0"); ... addHorizontalSlider("speed", ...)`),
// exactly like before the "fix". Root cause: Faust's par(...) combinator is
// a CODE-GENERATING combinator, not a runtime loop -- passing an expression
// (clearAllGlobal, itself a button() box) as an argument to oneLooper, which
// par() instantiates 20 times, means that expression is substituted/inlined
// at each of the 20 call sites. Each inlined copy of the button()/hslider()
// box gets its own UI declaration, and since the call site is lexically
// inside vgroup("looper%2i"), each inlined declaration lands inside that
// specific looper's group too. Hoisting the TEXT of the declaration outside
// the par loop doesn't matter: what determines UI-zone identity in Faust is
// where the box is ELABORATED (i.e. every place the expression is used),
// not where it is textually written once in the source. There is no Faust
// mechanism for "declare this UI control once, reference the same zone from
// many call sites" when the reference crosses a par() replication boundary --
// the language does not have that concept; par() genuinely duplicates
// whatever signal graph (including UI primitives) sits in its body.
//
// REAL FIX: stop declaring clear/speed as Faust UI controls (hslider/button)
// at all. Make them plain SIGNAL inputs to process() instead -- exactly the
// same technique already proven for prevFiltIn below (a native block-rate
// value pushed in from audio_thread.cpp's fins[] array, never a UI zone).
// A signal input threaded through par() is NOT re-elaborated per instance
// the way a UI-primitive box is -- it's just a wire, so every oneLooper
// instance genuinely reads the exact same sample-accurate value every block,
// with no zone-lookup, no FaustUI::set() suffix-matching, and no possibility
// of the compiler duplicating it. audio_thread.cpp now writes cmd/clearall
// and the computed speed multiplier directly into fins[2]/fins[3] (constant
// across the block) instead of calling fui.set("clear"/"speed", ...).

// prevFiltIn: previous block's FULLY-EFFECTED mix output (audio_thread.cpp's
// prevFiltOut, dsp/aloop.dsp's recordTap/4th process() output), native
// one-block-lag fold-in -- the same proven technique as the old glitchIn tap
// (see dsp/aloop.dsp's top-of-file comment for why this is a DEDICATED
// record-only input rather than being folded into `in`/`fin`: adding it to
// the engine's live/dry input would make it flow into `fx` again next block,
// re-entering every stage -- the WITNESSED feedback whine, see
// audio_thread.cpp's "REVERTED here" comment, now would apply to the WHOLE
// fx chain not just microStage). Routed ONLY into the record capture term
// below: recording now ALWAYS captures the fully-effected signal
// (pitch/delay/reverb/microrepeat/filters), matching the user's explicit
// requirement ("all effects should record"), not just raw dry input.
// REPLACES the old (in + glitchIn) composition entirely -- `in` (raw pre-fx
// input) is no longer part of the record term at all, since prevFiltIn one
// block later already contains everything `in` would have contributed (this
// block's `in` becomes part of next block's filtOut via the normal fx path),
// and the old glitchIn term is redundant now that prevFiltIn already carries
// post-glitch content (effects_runtime.dsp: microStage feeds both
// filterStage and rawGlitchTap, so microStage's output is upstream of and
// already baked into filtOut/recordTap).
oneLooper(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen) = out : attachLevel
with {
    recN  = button("rec");
    playN = checkbox("play");
    lenN  = hslider("len", 48000, 64, MAXLEN, 1);
    volN  = hslider("vol", 1.0, 0.0, 1.0, 0.001);
    // FINISH-QUANTIZATION (user, this turn): "when our second loop is short,
    // it doesnt take the start and stop timing it its making it longer and
    // offsetting the position instead of matching it" / "when the loops are
    // longer or shorter than the first loop it should snap to the most
    // sensible in-phrase multiple or division" / "it should wait when
    // waiting is closer, and backdate if pressed just too late instead of
    // waiting for the next one." ROOT CAUSE: wrapLen previously latched from
    // writeIdx's RAW elapsed sample count at finishEdge -- the DSP never
    // read the nicely-quantized target apc_grid.cpp computes, so a short
    // second loop played back at its raw (un-quantized, often longer)
    // length with a phase anchor that didn't match either. FIX: two new
    // per-looper zones -- finishReqN (a button C++ pulses at the ACTUAL
    // finish press, BEFORE releasing recN) and finishTargetN (the chosen
    // quantized target sample count, pushed in the SAME instant). Once a
    // finish is requested, writing continues (ignoring recN's own release)
    // until writeIdx reaches finishTargetN -- extending recording sample-
    // accurately if the target hasn't been reached yet ("wait when waiting
    // is closer"), or stopping essentially immediately if the target was
    // already passed at request time ("backdate if pressed just too late":
    // writeIdx is already >= finishTargetN the instant it's checked, so
    // writing halts within one sample of the request; the few extra samples
    // already written past the target during the raw take are simply never
    // read back once wrapLen latches to the SMALLER finishTargetN value --
    // no erase/undo needed, an unreachable ring region beyond wrapLen-1 is
    // harmless). apc_grid.cpp computes the direction (extend vs backdate)
    // by finding the nearest quantization candidate to the raw elapsed
    // duration -- this file only needs to know the FINAL target, not which
    // direction it came from.
    finishReqN    = button("finishreq");
    finishTargetN = hslider("finishtarget", 0, 0, MAXLEN, 1);
    eraseN = button("erase");   // per-looper wipe (hardware ERASE_TRACK 0x60)
    // wipe this loop when EITHER the global clear or this looper's erase is held.
    wipe   = max(clearAll, eraseN);

    // ---- WRITE side ----
    // ROOT CAUSE (silent second recording after clear, reported since project
    // start): writeIdx was a FREE-RUNNING counter with NO reset anywhere --
    // not at program start, not on wipe/clear, not on a fresh ARM. It just
    // kept incrementing modulo wrapLen forever from whatever arbitrary
    // position it happened to be at. Two compounding problems:
    //   1. On the FIRST-establish FINISH (apc_grid.cpp's applyRecPlayCycle,
    //      masterLen(before)==0 branch), `len` is set for EVERY looper
    //      in-place, changing wrapLen INSTANTLY the next Faust block. A
    //      writeIdx that was wrapping at the OLD wrapLen (e.g. the
    //      Faust-compiled default 48000) is suddenly modulo'd against a
    //      DIFFERENT wrapLen -- `(prev+1) % newWrapLen` does not restart at
    //      0, it just continues from wherever `prev` was, now bounded by a
    //      different modulus (harmless numerically -- % is always in-range
    //      -- but the value is essentially arbitrary relative to "start of
    //      loop", never guaranteed to be near 0).
    //   2. Nothing ties writeIdx==0 to the ARM press (rec 0->1). So a fresh
    //      recording's write span starts at whatever position the
    //      free-running counter is at that exact block -- e.g. index 743 --
    //      NOT index 0. The READ side's readPos is a SEPARATELY initialized/
    //      advancing accumulator with NO relationship to where the write
    //      actually started. Even though both spans are wrapLen samples
    //      long, they are not aligned: readPos scans from wherever IT starts
    //      (also arbitrary pre-fix), so it can read pre-write stale/zeroed
    //      ring content for part of the loop and real content for the rest,
    //      or (worst case) mostly stale content if the misalignment is bad
    //      -- exactly matching "second recording captures nothing".
    // FIX: detect the ARM edge (rec 0->1, same engageEdge/counter-reset
    // pattern as effects/home/faust/microrepeat.dsp's engageEdge/sampleIdx)
    // and force writeIdx back to a KNOWN position (0) at that exact instant,
    // via ba.if in the recursion's own reset condition -- same idiom as
    // microrepeat's `counter(prev) = ba.if(engageEdge, 0, prev+1)`. This
    // guarantees every fresh recording's write span starts at a known,
    // read-reachable position, regardless of how long writeIdx had been
    // free-running before, and regardless of any wrapLen change that
    // happened in between (a stale `prev` value no longer matters once the
    // edge fires -- it's discarded, not carried forward).
    // wipe (clear-all/erase) does NOT need its OWN separate writeIdx reset:
    // wipe already zeroes writeVal (nothing new gets written while wiped),
    // and the NEXT genuine recording after a wipe always goes through a
    // fresh ARM (recN 0->1) first -- which this same edge already resets to
    // 0. A redundant wipe-triggered reset would be a no-op given ARM always
    // follows wipe before any real write happens again.
    recPrev = recN : mem;
    // ARM-QUANTIZATION (user, this turn): "quanting recordings to multiples,
    // not 2/3 etc" -- every recording's actual START should land exactly on
    // a sub-phrase GRID TICK (a power-of-2 division of the shared master
    // phrase, masterLen/16), not fire immediately on press, mirroring
    // ../looper's real track_latch mechanism (loopMachine.cpp:1081-1087,
    // confirmed via cross-codebase research this session: "a pending
    // RECORD only latches when masterPhase % gridStep == 0"). armPulse is
    // the RAW C++ press edge (recN 0->1, renamed from the old armEdge --
    // recN itself still flips to 1 immediately on press, unchanged C++
    // button semantics); armPending latches "waiting for the next grid
    // tick" until the REAL armEdge (below) actually fires. EXCEPTION: the
    // very FIRST recording ever on a clear rig (masterLen==0, no grid
    // exists yet to snap to) arms IMMEDIATELY and unquantized -- it is the
    // one that DEFINES the grid, matching ../looper's own design where the
    // first clip always defines the phrase, later clips align to it.
    armPulse = (recN > 0.5) & (recPrev < 0.5);
    // gridStep: masterLen/16, a power-of-2 (1/16) division of the shared
    // phrase -- genuinely a "multiple", never an arbitrary fraction like
    // 1/3, matching the user's explicit requirement. Floored at 1 sample so
    // a pathologically short masterLen can never produce a zero/negative
    // step. wrapAbs (defined further below, in the READ side) is reused
    // here too -- both this grid-tick detector and absPos's own wrap need
    // the identical "wrap p into [0,len)" arithmetic, so it's hoisted to a
    // shared helper rather than duplicated (see its own definition).
    gridStep = max(1.0, masterLen / 16.0);
    phaseInGrid = wrapAbs(masterPhase, gridStep);
    phaseInGridPrev = phaseInGrid : mem;
    // A grid tick was just CROSSED the instant phaseInGrid wraps backward
    // (from near-gridStep back down near 0) -- the same "detect a wrap by
    // comparing against the previous sample" idiom armPulse/finishEdge
    // already use, just applied to a continuously-wrapping phase instead of
    // a binary control signal.
    gridTickCrossed = phaseInGrid < phaseInGridPrev;
    // armPending/armEdge: same same-instant-cycle risk already caught once
    // this session (finishRequested/finishEdge/writeIdx) -- armEdge must
    // NOT depend on armPending's CURRENT-instant value if armPending's own
    // recursion in turn depends on armEdge, or the two become mutually
    // recursive. Fixed the identical way: armPendingStep folds its OWN
    // reset condition inline using `prev` (its own recursion parameter,
    // i.e. armPending's PREVIOUS-sample value) rather than referencing the
    // outer `armEdge` identifier, and armEdge itself is derived from
    // armPending's OUTPUT via a one-sample-delayed read (`: mem`), the same
    // edge-detection idiom used for recPrev/finishRequested elsewhere in
    // this file -- never a same-instant circular read.
    armPendingStep(prev) = ba.if(masterLen < 0.5, 0,
                            ba.if(prev & gridTickCrossed, 0, ba.if(armPulse, 1, prev)));
    armPending = armPendingStep ~ _;
    armPendingPrev = armPending : mem;
    armEdge = ba.if(masterLen < 0.5, armPulse, armPendingPrev & gridTickCrossed);
    // finishRequested: latches the instant a finish is PULSED (finishReqN,
    // pushed by apc_grid.cpp the same block as the finish press, alongside
    // finishTargetN), stays true until the NEXT armEdge. Its own recursion
    // depends only on armEdge/finishReqN -- no reference to writeIdx, so it
    // cannot participate in any writeIdx-adjacent mutual-recursion class
    // (the exact RMW-class rejection already hit once this session tracing
    // wrapLen<->writeIdx).
    finishRequestedStep(prev) = ba.if(armEdge, 0, ba.if(finishReqN > 0.5, 1, prev));
    finishRequested = finishRequestedStep ~ _;
    // ROOT CAUSE (silence ever since TRUE varispeed's rwtable redesign,
    // WITNESSED live: "can't hear our loops at all, but we can hear half a
    // loop the first time pressing varispeed"): the PRIOR fix (0260de2/
    // 1099e78) latched wrapLen at armEdge (recording START) from `lenN` --
    // but `lenN` (the shared master length) is only ever computed and
    // written by the native shell AFTER a recording finishes
    // (apc_grid.cpp's applyRecPlayCycle, from the just-finished take's own
    // elapsed duration). At ARM time, `lenN` can only ever reflect the
    // PREVIOUS recording's length, or -- for the very first recording ever
    // on a clean boot -- Faust's compiled-in hslider default (48000 = 1s),
    // regardless of how long the user actually holds record for. So the
    // write pass wrapped every 48000 samples (or whatever the stale prior
    // length was) throughout the WHOLE recording, populating only a
    // fraction of that span (or overwriting it, if held longer), while
    // readPos -- ungated during recording, just inaudible via hold's
    // (1-recN) gate -- kept drifting through that same stale span. The
    // instant FINISH set play=1, playback started scanning from wherever
    // readPos happened to have drifted to: mostly the stale span's
    // zero-init silence, briefly clipping through the one small region that
    // genuinely got written -- exactly "half a loop, once, then silence".
    // FIX: stop depending on lenN/the native shell's timing entirely.
    // writeIdx itself, counted up FREELY (not modulo any wrapLen) during an
    // active recording, already equals the exact number of samples written
    // by the time FINISH fires -- a value entirely internal to this Faust
    // instance, with no cross-thread race. Latch wrapLen from writeIdx's own
    // value at finishEdge instead of from lenN at armEdge. writeIdx is
    // reset to 0 at armEdge as before (so it starts counting from a known
    // position every take), but during the ARM..FINISH span it must NOT
    // wrap modulo the OLD wrapLen anymore (that was the actual bug) --
    // instead it counts up freely, bounded only by MAXLEN (a recording
    // longer than that is simply clamped, matching the old ring's own
    // capacity ceiling). Faust-legal: an ordinary internal `~` recursion,
    // entirely local to this oneLooper instance, no new UI control, no
    // cross-stage recursion.
    wrapLenStep(prev) = ba.if(finishEdge, writeIdxForLatch, prev);
    wrapLen = max(1, wrapLenStep ~ _);
    // WITNESSED live: "we caught it phrasing in non multiples repeating 2
    // and 1/2 times etc" -- ROOT CAUSE: in the BACKDATE case (raw recording
    // already past the quantized target when finish was requested),
    // recordingGate correctly stops writeIdx from advancing further, but
    // writeIdx's HELD value is whatever it happened to be AT THAT INSTANT
    // (the overshot raw count), never pulled back down to the clean
    // finishTargetN -- so wrapLen latched to an arbitrary, un-quantized
    // length instead of the intended clean multiple/division, exactly
    // matching "repeating 2 and 1/2 times" (a wrapLen that's ~2.5x the
    // intended target instead of landing on 2x or 4x). FIX: prefer
    // finishTargetN directly over writeIdx's raw value whenever a finish
    // was genuinely requested (finishRequested) -- correct for BOTH cases
    // uniformly: in the extend case writeIdx already lands exactly on
    // finishTargetN by construction (recordingGate's own stop condition),
    // so this is a no-op there; in the backdate case it's what actually
    // fixes the bug, discarding the overshot raw count in favor of the
    // clean target. Falls back to raw writeIdx only if no finish was ever
    // requested (should not happen in normal operation now that
    // apc_grid.cpp always pulses finishreq, but keeps this file correct if
    // some future caller ever skips it).
    // (fallback-path note: when finishRequested is false, writeIdx's
    // CURRENT value is used directly -- safe because writeIdx's own
    // recursion, defined below, uses `wrapLen` only for the NOT-recording
    // (idle) case; while `recN` is held, writeIdx counts up
    // unconditionally, so by the time finishEdge fires in this fallback
    // path, writeIdx already holds the exact elapsed sample count for this
    // take, with no additional latching needed.)
    writeIdxForLatch = ba.if(finishRequested, finishTargetN, writeIdx);
    // WRITE side: counts up from 0 (reset at armEdge) while actively
    // recording, uncapped by any wrapLen (that modulus isn't known yet --
    // it's only DERIVED from this same counter once FINISH latches it
    // above); clamped to MAXLEN-1 so a take longer than the ring's own
    // capacity simply stops advancing rather than wrapping and overwriting
    // its own start (matching the old ring's hard capacity ceiling). Once
    // NOT recording (recN==0, after FINISH), writeIdx's value is dead --
    // nothing writes again until the next armEdge resets it -- so the idle
    // branch just HOLDS the last value (no wrapLen reference at all) rather
    // than wrapping it: referencing wrapLen here would make wrapLen and
    // writeIdx MUTUALLY recursive (wrapLen's own recursion above reads
    // writeIdx via writeIdxForLatch), which Faust's evaluator genuinely
    // cannot resolve -- WITNESSED live via CI: "after 5200 evaluation
    // steps, the compiler has detected an endless evaluation cycle of 19
    // steps", the exact RMW-class rejection this file's own top-of-file ADR
    // already documents for a different, earlier attempt. Holding (not
    // wrapping) the idle value is semantically identical for this file's
    // purposes: idle writeIdx is provably dead (nothing reads or writes
    // through it again until the next armEdge unconditionally resets it to
    // 0), so what it holds in between literally cannot matter. (This
    // "idle" state is now recordingGate==false, not simply recN==0 -- see
    // recordingGate below, which also stays true a little past recN's own
    // release while a finish is pending its quantized target.)
    // recordingGate(prev): should writeIdx keep counting THIS sample? Two
    // ways to still be "recording": recN is genuinely held (the normal
    // case, matches pre-quantization behavior exactly when no finish has
    // been requested yet), OR a finish HAS been requested but writeIdx
    // hasn't yet reached its target (this is what lets writing continue
    // past recN's own release -- "wait when waiting is closer"). Written in
    // terms of `prev` (writeIdx's own PREVIOUS value, the recursion's own
    // parameter) rather than re-reading the `writeIdx` identifier itself --
    // this is the fold-into-one-recursion design that avoids the
    // writeIdx<->finishRequested cycle a separate "writingActive" signal
    // would have created (traced explicitly this session before writing
    // any code).
    recordingGate(prev) = (recN > 0.5) | (finishRequested & (prev < finishTargetN));
    writeIdxStep(prev) = ba.if(armEdge, 0,
                          ba.if(recordingGate(prev), min(prev + 1, MAXLEN - 1), prev));
    writeIdx = writeIdxStep ~ _;
    // finishEdge: the REAL stop instant that latches wrapLen/
    // recordStartPhaseOffset -- derived from recordingGate's OWN falling
    // edge (evaluated against writeIdx's actual output via `: mem`, the
    // identical edge-detection idiom already used for recN/recPrev above,
    // NOT a new recursion reading writeIdx circularly). "Backdate if
    // pressed just too late": if finishTargetN was ALREADY <= writeIdx the
    // instant finishReqN pulsed, recordingGate is false on the very next
    // sample (prev is already >= finishTargetN), so finishEdge fires
    // essentially immediately -- the few extra samples already written past
    // the target during the raw take are simply never read back once
    // wrapLen latches to the smaller finishTargetN value (an unreachable
    // ring region beyond wrapLen-1 is harmless, no erase/undo needed).
    recordingGateNow = recordingGate(writeIdx : mem);
    recordingGatePrev = recordingGateNow : mem;
    finishEdge = (recordingGateNow < 0.5) & (recordingGatePrev > 0.5);
    // record: capture the PREVIOUS block's fully-effected mix (prevFiltIn),
    // one-block-lag, so every recording is ALWAYS effected (pitch/delay/
    // reverb/microrepeat/filters) -- matching the user's explicit
    // requirement, not just live input passed through raw. This also
    // captures glitch/microrepeat content (matching looper's "stutter
    // becomes ... the record source", loopMachine.cpp:806-833) since
    // prevFiltIn already contains microStage's output one block later, and
    // captures SHIFT/glitch-held loop content too, since the native fold
    // already routes that content through `fx` into filtOut before this tap
    // is taken. prevFiltIn never touches `in`/dry, only this record term, so
    // it structurally cannot re-enter `fx` on any later block. `wipe` zeroes
    // the write too (a wiped/erased loop must not silently resurrect old
    // ring content next read pass), matching the old hold*(1-wipe) gating.
    // Gated by recordingGateNow (NOT raw recN): recN is released to 0
    // immediately at the finish press, but writeIdx may keep advancing a
    // little longer to reach its quantized target -- gating writeVal on
    // recN alone would write SILENCE into those extended index positions
    // instead of real audio, corrupting exactly the samples the
    // finish-quantization extension exists to capture.
    writeVal = prevFiltIn * recordingGateNow * (1.0 - wipe);
    ring = rwtable(MAXLEN, 0.0, writeIdx, writeVal, readIdx0);

    // ---- READ side: PHRASE-LOCK (user's standing requirement: "our loops
    // must stay perfectly in phrase... the phrase should always be correct
    // even if a repeat or varispeed was hit" / "there is no looper sync at
    // all right now, they're independent"). ROOT CAUSE of the reported
    // drift: readPos was a purely SELF-INTEGRATING accumulator with no tie
    // back to any shared reference -- two loopers recorded at different
    // times, or one that had a glitch/repeat/varispeed engagement nudge its
    // own accumulator differently than another's, could drift apart from
    // each other forever with nothing to ever resync them. FIX: derive
    // position from `masterPhase` (a process()-level SIGNAL INPUT, see
    // effSpeed's own comment for why -- native audio_thread.cpp computes a
    // single sample-accurate, Link-phase-corrected counter ONCE per block
    // and pushes it in as a plain wire, exactly like effSpeed/clearAll, so
    // par() cannot duplicate it into 20 zones) plus a per-looper ANCHOR
    // (recordStartPhaseOffset, latched at THIS looper's own finishEdge from
    // masterPhase's value at that exact instant) -- mirrors ../looper's real
    // design (Looper.h/loopClip.cpp/loopClipUpdate.cpp, confirmed via
    // cross-codebase research this session): every clip's position is
    // ((masterPhase - itsOwnRecordStartOffset) mod itsOwnLength), a PURE
    // FUNCTION recomputed fresh, not an independently-integrated value --
    // so two loopers anchored to the SAME masterPhase can never drift
    // apart from each other regardless of how long they've been playing,
    // glitch engagement, or repeat presses (none of which touch masterPhase
    // or any looper's own offset).
    recordStartPhaseOffsetStep(prev) = ba.if(finishEdge, masterPhase, prev);
    recordStartPhaseOffset = recordStartPhaseOffsetStep ~ _;
    wrapAbs(p, len) = p - floor(p / float(len)) * float(len);
    absPos = wrapAbs(masterPhase - recordStartPhaseOffset, wrapLen);
    // effSpeed is a plain process()-level SIGNAL INPUT (see ROOT CAUSE
    // comment above oneLooper for why it must never be a UI hslider/button
    // threaded through par()). Clamp to a sane nonzero range so a
    // pathological Link-tempo ratio or manual speed can never stall (0) or
    // explode (runaway) the read accumulator.
    speedClamped = max(0.1, min(8.0, effSpeed));
    // VARISPEED INTERACTION: at effSpeed==1.0 (normal playback, the common
    // case), position is ALWAYS the pure masterPhase-relative formula above
    // -- zero drift by construction, never self-integrated. Varispeed
    // (effSpeed != 1.0) is a deliberate, momentary DEVIATION from the
    // master's own rate (that is what varispeed means -- playing faster or
    // slower than the shared clock) -- while engaged, this looper falls
    // back to its own self-integrating accumulator (the only way to express
    // "play at a different rate than the master"), but the INSTANT effSpeed
    // returns to 1.0, it re-snaps to the pure absPos formula on the very
    // next sample, killing any drift accumulated while varispeed was held --
    // mirrors ../looper's own m_playPos, which is likewise kept synced to
    // the master-derived position every block while rate==1 and only
    // self-integrates during an actual rate change (loopClipUpdate.cpp,
    // confirmed via cross-codebase research this session).
    varispeedActive = (effSpeed < 0.999) | (effSpeed > 1.001);
    readPosStep(prev) = ba.if(armEdge | finishEdge, absPos,
                         ba.if(varispeedActive, wrapAbs(prev + speedClamped, wrapLen), absPos));
    // HISTORY (superseded, kept for context): this file previously carried
    // readposdiag/wraplendiag TEMPORARY diagnostic hbargraphs, added to
    // investigate "plays a part of the loop once, does not repeat" by
    // exposing readPos/wrapLen as hbargraph OUTPUTS read back via fui.get()
    // in audio_thread.cpp. Two attempts to make Faust's dead-code
    // elimination actually keep them reachable from `out` were tried (the
    // first attach()-chaining attempt still failed since wrapLen didn't
    // structurally depend on the attached signal); both attempts still read
    // fui.get()'s -1.0 "not found" default live, every single time.
    // REMOVED (this fix): both diagnostic hbargraphs are gone -- they were
    // the newest, least-proven code sitting DIRECTLY in the
    // real readPos chain that feeds readIdx0/readIdx1 -- i.e. every reader of
    // this signal path. Faust's attach() is documented/verified to be a
    // genuine value-identity pass-through (attach(x,y) returns x unchanged;
    // this was independently re-verified against Faust's own semantics while
    // investigating this exact regression), so the diag chain itself was not
    // the direct cause of the silence bug fixed above -- but a broken
    // diagnostic that has never once produced a real reading, sitting inline
    // on the single most fragile signal in this file, is a standing risk for
    // zero ongoing benefit. Removed entirely rather than attempted a third
    // time; if readPos/wrapLen visibility is needed again, prefer exposing
    // them via the EXISTING working telemetry path (audio_thread.cpp's
    // fui.get() reads of already-functioning zones like level/rec/play)
    // rather than a new attach()-chained hbargraph.
    readPos = readPosStep ~ _;
    readIdx0 = int(readPos) % wrapLen;             // floor tap
    readIdx1 = (readIdx0 + 1) % wrapLen;             // ceil tap, wrapped
    readFrac = readPos - floor(readPos);
    // Second read of the SAME ring at the ceil index for linear
    // interpolation. Both reads are plain lookups (no write), so this is
    // NOT a read-modify-write -- the ring's only writer is writeIdx/writeVal
    // above, entirely independent of where the read side is looking.
    ringCeil = rwtable(MAXLEN, 0.0, writeIdx, writeVal, readIdx1);
    delayed = ring + (ringCeil - ring) * readFrac;   // linear-interpolated read
    // hold/recirculate exactly as before -- the wrap-around READ POSITION is
    // what changes speed now (not effLen), so hold's own gating (not
    // recording, not wiped) is unchanged from the old design, EXCEPT it now
    // uses recordingGateNow instead of raw recN for the same reason as
    // writeVal above: recording (and therefore "not recording" muting of
    // playback) must track the EXTENDED finish-quantization window, not
    // just recN's own early release.
    hold = delayed * (1.0 - recordingGateNow) * (1.0 - wipe);
    // record (live monitoring term): the OLD de.fdelay-ring design's `out`
    // was `step = record+hold` fed straight back into the delay -- since
    // write and read were the SAME point in that ring, recording produced
    // zero-added-latency monitoring (you hear yourself as you record) for
    // free. The new independent read/write heads lose that automatically
    // (the read position is generally somewhere else in the ring while
    // writeIdx advances), so this term reinstates it explicitly: while
    // actively recording, `out` carries the live writeVal directly, exactly
    // matching the old ring's same-point read=write behavior.
    record = writeVal;
    loopSig = record + hold;
    out = loopSig * playN * volN;
    // LEVEL meter: an hbargraph UI OUTPUT (never fui.set() from ParamStore --
    // read-only via fui.get(), same pattern as the existing rec/play/vol
    // telemetry reads in audio_thread.cpp), fed via Faust's attach() idiom so
    // the meter signal rides along the real audio signal without adding a
    // second audible output channel. Raw abs-peak magnitude in the looper's
    // own [0,1] float range (not dB, not looper's raw s32 scale) -- matching
    // looper's vuLow/vuMid/vuHigh thresholds means the C++ side documents its
    // own equivalent thresholds in aloop's normalized range (see apc_leds.cpp).
    // ba.slidingMax gives a fast-rise/slow-decay envelope (a real "peak
    // meter" shape rather than a raw instantaneous sample, which would
    // flicker the LED color every block) over a ~4096-sample (~85ms @48kHz)
    // window.
    levelMeter = hbargraph("level", 0.0, 1.0);
    // ba.slidingMax(n, maxN): sliding window max over the last n samples,
    // maxN is the compile-time buffer-size bound (n <= maxN). Window ~4096
    // samples (~85ms @48kHz), bound equal to the window since it's a fixed
    // compile-time constant here, not runtime-variable.
    // writeidx telemetry: exposes writeIdx's CURRENT sample-accurate value
    // read-only via fui.get() (same working pattern as levelMeter above --
    // NOT the readposdiag/wraplendiag idiom that failed twice earlier this
    // session, since attach()-chaining directly onto the real output signal
    // is the one proven to actually survive Faust's dead-code elimination:
    // the hbargraph's argument must GENUINELY derive from the attached
    // signal, not merely be evaluated nearby). Lets apc_grid.cpp read the
    // TRUE elapsed sample count since the real (grid-quantized) arm instant
    // at the moment a finish is requested -- compensating for
    // ARM-quantization's press-to-grid-tick timing gap in the
    // finish-quantization math, instead of estimating duration from
    // wall-clock press-to-press timing alone (which would be biased by
    // however long the grid-tick wait took). writeIdx itself doesn't
    // mathematically depend on `out`, so it's folded in via a genuine
    // (if numerically inert, *0) dependency on `out` -- the SAME technique
    // wrapLenDiag used successfully once fixed (attach(x, x*0+something :
    // hbargraph)), proven this session to survive DCE when the dependency
    // is real, unlike the FIRST (rejected) readposdiag attempt.
    writeIdxMeter = hbargraph("writeidx", 0.0, float(MAXLEN));
    attachWriteIdx(x) = attach(x, x*0.0 + float(writeIdx) : writeIdxMeter);
    attachLevel(x) = attach(x, abs(x) : ba.slidingMax(4096, 4096) : levelMeter) : attachWriteIdx;
};

// The engine outputs (dry-thru, loop-sum) SEPARATELY rather than pre-summed, so
// the caller (aloop.dsp) can implement looper's SHIFT-held monitor-fold: fold
// the loop-sum into the effect chain's input while complementarily suppressing
// the dry loop contribution at the final mix (loopMachine.cpp:709-730's
// g_fold/g_dry crossfade). `par` with a "%2i"-labelled vgroup gives each
// instance its own addressable controls (looper00/rec … looper19/rec).
//   in -> [ thru , (looper0 + looper1 + … + looper19) ]
// vgroup label "looper%2i" → Faust substitutes the par index, giving group names
// "looper 0" … "looper19" (a space for single digits). The native shell's
// targetToZone normalizes to this exact form so each looper is addressable.
// Second process() input (prevFiltIn, previous block's fully-effected mix
// output) is broadcast to every looper's record-only tap (see oneLooper's
// comment) -- it never appears in the dry-thru output (only `in` does), so
// the fully-effected signal is recordable but never re-enters the live/dry
// path (which would flow back into `fx` next block).
// clearAllGlobal/speedMulGlobal are evaluated ONCE here, OUTSIDE any vgroup,
// then passed as ordinary parameters into every oneLooper instance -- this is
// what actually makes them single shared zones (see oneLooper's comment above
// for why referencing them by bare name from INSIDE the vgroup-wrapped par
// failed to do this).
// clearAll/effSpeed/masterPhase are all genuine process() SIGNAL inputs (see
// the ROOT CAUSE comment above oneLooper) -- plain wires, not UI-declared
// button()/hslider() boxes, so par()'s per-instance code generation cannot
// duplicate them into 20 separate zones. They are broadcast identically to
// every oneLooper instance the same way prevFiltIn already is. effSpeed
// REPLACES the old speedMul: it is audio_thread.cpp's precomputed product of
// the manual half/double-speed multiplier AND the Link-tempo-driven ratio
// (recordedBpm/currentLinkBpm), matching looper's
// `effectiveRate = m_playRate * g_globalSpeedMul` exactly (see top-of-file
// comment) -- one combined signal, since Faust's read accumulator only ever
// needs the FINAL rate, not the two factors separately. masterPhase is the
// PHRASE-LOCK shared clock (see oneLooper's own comment on
// recordStartPhaseOffset/absPos): audio_thread.cpp computes a single
// sample-accurate, Link-phase-corrected counter once per block and pushes it
// in here, so every looper anchors its own recordStartPhaseOffset to the
// SAME reference and can never drift apart from another looper regardless
// of glitch/repeat/varispeed engagement.
loopEngine(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen) = in, (par(i, NLOOPERS, vgroup("looper%2i", oneLooper(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen))) :> _);

process(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen) = loopEngine(in, prevFiltIn, clearAll, effSpeed, masterPhase, masterLen);   // (dry, loopSum) — two outputs, see aloop.dsp's fold mix
