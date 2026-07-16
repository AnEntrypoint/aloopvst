// LooperEngine::processBlock — ported line-for-line from aloop's
// src/dsp/audio_thread.cpp worker() function (the per-block DSP section only
// -- the ALSA open/hw_params/PCM read-write scaffolding around it does not
// apply to a plugin, since JUCE's processBlock already IS the per-block
// callback a host drives). Every comment below that references a "WITNESSED"
// bug or a "REGRESSION FOUND AND FIXED" note is preserved verbatim from the
// original because it documents load-bearing ordering/gating this port must
// not silently change -- see docs/ARCHITECTURE.md and
// docs/CLONE-PARITY-REFERENCE.md.
//
// What's genuinely different from audio_thread.cpp here:
//   - No ALSA S32_LE/S16_LE wire format conversion -- JUCE hands us a plain
//     float AudioBuffer already in [-1,1], so the deinterleave/normalize math
//     collapses to a straight mono downmix (see PluginProcessor::processBlock).
//   - No Ableton Link: masterPhase/effSpeed's "Link resync" branch is replaced
//     by a host-transport resync (PluginProcessor passes hostBpm/
//     hostPpqPosition/hostIsPlaying, read via JUCE's AudioPlayHead). See the
//     "Master phase clock" section below for exactly how this substitutes.
//   - No per-core busy% timing (no SCHED_FIFO thread to profile) and no xrun
//     counter (the host's own audio callback owns over/underrun accounting).
#include "LooperEngine.h"

namespace aloopvst {

// Map a control-map TARGET name ("looper3/rec", "fx/hp") to the Faust zone
// label the home stack exposes -- identical to audio_thread.cpp's
// targetToZone (see that file's own comment for the exact rationale).
std::string LooperEngine::targetToZone(const std::string& target) {
    if (target.rfind("looper", 0) == 0) {
        auto slash = target.find('/');
        if (slash != std::string::npos) {
            int idx = atoi(target.c_str() + 6);
            char z[64];
            snprintf(z, sizeof z, "looper%2d/%s", idx, target.c_str() + slash + 1);
            return z;
        }
    }
    if (target == "fx/hp")      return "HPCUT";
    if (target == "fx/lp")      return "LPCUT";
    if (target == "fx/lpres")   return "LPRES";
    if (target == "fx/reverb")  return "REVAMT";
    if (target == "fx/delay")   return "DELAYAMT";
    if (target == "fx/time")    return "TIME";
    if (target == "fx/formant") return "FORMANT";
    if (target == "fx/pitch")   return "SEMIS";
    // fx/monitorfold has NO Faust zone anymore -- the SHIFT-held fold is a
    // native block-rate mix (see the prevLoopSum_/foldGain_ code below), so
    // it's read directly via params_->get("fx/monitorfold") rather than
    // pushed into any Faust zone.
    return "";
}

void LooperEngine::processBlock(const float* in, float* out, int n,
                                 double hostBpm, double hostPpqPosition, bool hostIsPlaying) {
    const int N = n;
    FaustUI& fui = *fui_;
    ParamStore* g_params = params_;

    // Apply the remappable controls: for each bound target the control map
    // set, push its current value into the matching Faust zone. Done once per
    // block from the atomic store -- no locks, no alloc.
    if (g_params) {
        g_params->forEach([&](const std::string& target, int){
            std::string zone = targetToZone(target);
            if (!zone.empty()) fui.set(zone.c_str(), g_params->get(target));
        });
        // Global commands (cmd/*) are NOT per-looper Faust zones -- they drive
        // the engine-wide clear/speed process() SIGNAL INPUTS directly.
        bool clearAllHeld = g_params->get("cmd/clearall") > 0.5f;
        std::fill(clearBuf_.begin(), clearBuf_.begin() + N, clearAllHeld ? 1.0f : 0.0f);
        // CLEAR_ALL also resets the locally-established phrase length so the
        // NEXT standalone recording re-establishes a fresh phrase from
        // scratch (matches looper's LOOP_COMMAND_CLEAR_ALL resetting
        // masterLoopBlocks to 0).
        if (clearAllHeld) {
            g_params->setByName("cmd/master_len", 0.0f);
            g_params->setByName("cmd/recorded_bpm", 0.0f);
        }
        // manualSpeedMul: PURELY the manual half/double-speed button state
        // (1.0/0.5/2.0) -- never touched by tempo/host-transport code.
        float manualSpeedMul = 1.0f;
        if (g_params->get("cmd/halfspeed")   > 0.5f) manualSpeedMul = 0.5f;
        if (g_params->get("cmd/doublespeed") > 0.5f) manualSpeedMul = 2.0f;
        // Global STOP-ALL: clear every looper's play checkbox so all playback
        // stops. Per-looper stop is already covered by binding a control to
        // looper<i>/play (=0 stops that one); this is the single all-tracks
        // command. Edge-triggered on the held value so it doesn't fight a
        // user re-arming a looper.
        if (g_params->get("cmd/stopall") > 0.5f) {
            char z[32];
            for (int lp = 0; lp < kLooperCount; lp++) {
                snprintf(z, sizeof z, "looper%2d/play", lp);
                fui.set(z, 0.0f);
            }
        }
        // APC live-pitch (mod-wheel / absolute): a performance offset ON TOP
        // of the static SEMIS knob (fx/pitch), so add rather than overwrite --
        // releasing the mod-wheel (engaged=0) must fall back to the static
        // knob value, not silently zero it.
        float staticSemis = g_params->get("fx/pitch");
        if (g_params->get("fx/pitchbend_engaged") > 0.5f) {
            fui.set("SEMIS", staticSemis + g_params->get("fx/pitchbend"));
            fui.set("ENGAGED", 1.0f);
        } else {
            fui.set("SEMIS", staticSemis);
        }
        // Microrepeat latch -> the microStage's DIV zone.
        fui.set("DIV", g_params->get("fx/microrepeat_div"));

        // ---- Per-looper STATE telemetry -----------------------------------
        {
            char z[32];
            for (int lp = 0; lp < kLooperCount; lp++) {
                snprintf(z, sizeof z, "looper%2d/rec",  lp); telemetry_.looperRec[lp]  = fui.get(z) > 0.5f;
                snprintf(z, sizeof z, "looper%2d/play", lp); telemetry_.looperPlay[lp] = fui.get(z) > 0.5f;
                snprintf(z, sizeof z, "looper%2d/vol",  lp); telemetry_.looperVol[lp]  = fui.get(z, 1.0f);
                snprintf(z, sizeof z, "looper%2d/level", lp); telemetry_.looperLevel[lp] = fui.get(z, 0.0f);
                // writeIdx telemetry (ARM-QUANTIZATION compensation, see
                // dsp/loop.dsp's writeIdxMeter comment): the TRUE elapsed
                // sample count since the real (grid-quantized) arm instant --
                // ApcControlSurface reads this at the FINISH press to compute
                // rawSamples precisely instead of estimating from wall-clock
                // press-to-press timing.
                snprintf(z, sizeof z, "looper%2d/writeidx", lp); telemetry_.looperWriteIdx[lp] = fui.get(z, 0.0f);
            }
        }
        telemetry_.monitorMode = g_params->get("fx/monitorfold") > 0.5f;

        // ---- Host-transport-driven loop length (replaces Link's role) -----
        // aloop's original reads an Ableton Link session for a shared tempo
        // reference; a plugin instead has the host's own transport
        // (AudioPlayHead: bpm, ppq position, isPlaying), read by
        // PluginProcessor and passed in as hostBpm/hostPpqPosition/
        // hostIsPlaying. Mirrors the original's "linkDrivingLength" branch:
        // when host tempo is available and playing, size every loop from it
        // (one bar = 4 beats, rounded to whole samples) -- a musical phrase
        // is a whole number of beats, matching the original's Link-driven
        // resize. When not playing / no tempo, fall back to the LOCALLY
        // established phrase length (cmd/master_len, set by
        // ApcControlSurface's applyRecPlayCycle from the first recorded
        // loop's own duration) -- this fallback branch runs UNCONDITIONALLY
        // whenever host tempo isn't the active length driver (matching the
        // original's fix for a bug where nesting this inside `if (link)`
        // left microrepeat/glitch inert on a Link-absent session -- the same
        // class of bug would occur here if this fallback were nested inside
        // an "if host transport valid" check instead of running whenever the
        // host branch ISN'T actively driving length).
        const double kBeatsPerBar = 4.0;   // one musical phrase = one bar = 4 beats
        bool hostDrivingLength = false;
        if (hostIsPlaying && hostBpm > 1.0) {
            hostDrivingLength = true;
            double samplesPerBeat = (sampleRate_ * 60.0) / hostBpm;
            double lenSamples = samplesPerBeat * kBeatsPerBar;
            char z[32];
            for (int lp = 0; lp < kLooperCount; lp++) {
                snprintf(z, sizeof z, "looper%2d/len", lp);
                fui.set(z, (float)lenSamples);
            }
            // Microrepeat's MLB (masterLoopBlocks, effects_runtime.dsp): the
            // same phrase length expressed in DSP blocks.
            fui.set("MLB", (float)(lenSamples / N));
        }
        if (!hostDrivingLength) {
            float masterLen = g_params->get("cmd/master_len", 0.0f);
            fui.set("MLB", masterLen > 0.0f ? (masterLen / (float)N) : 0.0f);
        }

        // TRUE varispeed: host-tempo-driven READ RATE (distinct from the
        // loop-LENGTH resize above). Ports looper's per-clip formula via
        // aloop's own audio_thread.cpp adaptation: recordedBpm/currentBpm,
        // applied only while host transport is actively driving length (an
        // unsynced/non-playing session must not silently alter pitch).
        float speedRatio = 1.0f;
        if (hostDrivingLength) {
            float recordedBpm = g_params->get("cmd/recorded_bpm", 0.0f);
            if (recordedBpm > 1.0f && hostBpm > 1.0) {
                speedRatio = recordedBpm / (float)hostBpm;
            }
        }
        float effSpeed = manualSpeedMul * speedRatio;
        std::fill(speedBuf_.begin(), speedBuf_.begin() + N, effSpeed);
        telemetry_.effSpeed = effSpeed;

        // ---- MASTER PHASE CLOCK ---------------------------------------------
        // A single sample-accurate counter, incremented by N every block,
        // wrapping at the current shared phrase length -- every looper's
        // recordStartPhaseOffset anchors to this, guaranteeing two loopers
        // can never drift apart regardless of glitch/repeat/varispeed
        // engagement (see dsp/loop.dsp's oneLooper comment).
        //
        // Host-transport resync (replaces the original's Link
        // beatPhaseMicroBeats resync): when the host transport is actively
        // playing and driving length, periodically correct any accumulated
        // float/scheduling drift by re-deriving phase from the host's own
        // ppq position (converted into a fraction of our phrase in samples)
        // instead of a Link quantum fraction. Standalone (host not playing,
        // or no usable tempo), masterPhase is purely the free-running block
        // counter -- still fully functional, just without an external
        // reference to correct against, exactly mirroring the original's
        // "no Link session" standalone behavior.
        {
            float masterLen = g_params->get("cmd/master_len", 0.0f);
            if (masterLen > 0.0f) {
                if (hostDrivingLength) {
                    // hostPpqPosition is in quarter notes; one bar (4 beats)
                    // is our phrase unit (matching the lenSamples formula
                    // above), so the phrase-relative fraction is
                    // (ppq mod 4) / 4 -- convert directly to a fraction of
                    // masterLen (which IS that same one-bar length in
                    // samples) rather than assuming the two ever differ.
                    double ppqInBar = std::fmod(hostPpqPosition, kBeatsPerBar);
                    if (ppqInBar < 0.0) ppqInBar += kBeatsPerBar;
                    double phaseFrac = ppqInBar / kBeatsPerBar;
                    masterPhaseSamples_ = phaseFrac * (double)masterLen;
                } else {
                    masterPhaseSamples_ += (double)N;
                }
                masterPhaseSamples_ = std::fmod(masterPhaseSamples_, (double)masterLen);
                if (masterPhaseSamples_ < 0.0) masterPhaseSamples_ += masterLen;
            } else {
                masterPhaseSamples_ = 0.0;   // no phrase established yet -- hold at 0
            }
            // Ramp masterPhaseBuf_ smoothly WITHIN the block (each sample i
            // gets masterPhaseSamples_ + i, wrapped at masterLen) -- holding
            // it block-constant like clearBuf_/speedBuf_ would produce a
            // stepped/aliased readback pattern (audibly indistinguishable
            // from bitcrushing; see audio_thread.cpp's own WITNESSED note on
            // this exact regression). masterPhase is a real per-sample
            // POSITION signal, not a momentary/step control.
            if (masterLen > 0.0f) {
                for (int i = 0; i < N; i++) {
                    double p = masterPhaseSamples_ + (double)i;
                    p = std::fmod(p, (double)masterLen);
                    if (p < 0.0) p += masterLen;
                    masterPhaseBuf_[(size_t)i] = (float)p;
                }
            } else {
                std::fill(masterPhaseBuf_.begin(), masterPhaseBuf_.begin() + N, 0.0f);
            }
            std::fill(masterLenBuf_.begin(), masterLenBuf_.begin() + N, masterLen);
        }
    } else {
        // No ParamStore (should not happen once PluginProcessor wires it up,
        // but keep this path inert/safe rather than crashing).
        std::fill(clearBuf_.begin(), clearBuf_.begin() + N, 0.0f);
        std::fill(speedBuf_.begin(), speedBuf_.begin() + N, 1.0f);
        std::fill(masterPhaseBuf_.begin(), masterPhaseBuf_.begin() + N, 0.0f);
        std::fill(masterLenBuf_.begin(), masterLenBuf_.begin() + N, 0.0f);
    }

    // Copy host input into fin_ (mono, already float [-1,1] -- PluginProcessor
    // has already downmixed any multi-channel host buffer to mono before
    // calling this). inPeak telemetry mirrors audio_thread.cpp's own peak
    // meter.
    float inPeak = 0.0f;
    for (int i = 0; i < N; i++) {
        fin_[(size_t)i] = in[i];
        float a = std::fabs(in[i]);
        if (a > inPeak) inPeak = a;
    }
    telemetry_.inPeak = inPeak;

    // Snapshot dry input for the sampler CAPTURE path -- taken BEFORE
    // renderInto's playback voices are mixed into fin_, so a sample recording
    // can never contain this block's own just-triggered sample playback. The
    // SHIFT/glitch fold is applied to this buffer too, in the SAME fold loop
    // below, so captureFin_ ends up as "dry input + folded loop content, no
    // sampler voices" -- distinct from fin_ (dry input + sampler voices +
    // folded loop content, the correct DSP-facing signal). This ordering is
    // load-bearing: audio_thread.cpp's own history documents a self-recording
    // bug from getting it wrong (see sampler.h's own top-of-file comment).
    for (int i = 0; i < N; i++) captureFin_[(size_t)i] = fin_[(size_t)i];

    // Sampler PLAYBACK mix-in: voices are mixed in BEFORE the loop engine +
    // effects chain, so a played-back sample gets all effects and is
    // recordable by a loop under SHIFT fold. Capture (recording INTO a sample
    // slot) is handled SEPARATELY below, after the SHIFT/glitch fold.
    Sampler* sampler = samplerPtr_.get();
    for (int i = 0; i < N; i++) samplerBuf_[(size_t)i] = (int32_t)(fin_[(size_t)i] * 32768.0f);
    sampler->renderInto(samplerBuf_.data(), N);
    for (int i = 0; i < N; i++) fin_[(size_t)i] = (float)samplerBuf_[(size_t)i] / 32768.0f;

    // ---- SHIFT-held / GLITCH-held native one-block-lag fold ----------------
    // Fold the PREVIOUS block's RAW loop-engine output into this block's
    // input BEFORE the Faust compute() runs, so loop.dsp's record path (which
    // runs first inside that call) sees the folded signal. One-pole ramps
    // (foldGain_/glitchFoldGain_) toward the held/released target, matching
    // looper's own per-block step. Both fold gains are combined (summed,
    // clamped to 1.0) so a simultaneous SHIFT+glitch hold folds loop content
    // in at most once, not doubled.
    if (g_params) {
        float foldTarget = g_params->get("fx/monitorfold") > 0.5f ? 1.0f : 0.0f;
        const float kFoldStep = 1.0f / 16.0f;   // reach target over ~16 blocks
        float glitchFoldTarget = g_params->get("fx/microrepeat_div") > 0.5f ? 1.0f : 0.0f;
        for (int i = 0; i < N; i++) {
            if (foldGain_ < foldTarget)      { foldGain_ += kFoldStep / N; if (foldGain_ > foldTarget) foldGain_ = foldTarget; }
            else if (foldGain_ > foldTarget) { foldGain_ -= kFoldStep / N; if (foldGain_ < foldTarget) foldGain_ = foldTarget; }
            if (glitchFoldGain_ < glitchFoldTarget)      { glitchFoldGain_ += kFoldStep / N; if (glitchFoldGain_ > glitchFoldTarget) glitchFoldGain_ = glitchFoldTarget; }
            else if (glitchFoldGain_ > glitchFoldTarget) { glitchFoldGain_ -= kFoldStep / N; if (glitchFoldGain_ < glitchFoldTarget) glitchFoldGain_ = glitchFoldTarget; }
            float combinedFold = foldGain_ + glitchFoldGain_;
            if (combinedFold > 1.0f) combinedFold = 1.0f;
            fin_[(size_t)i] += prevLoopSum_[(size_t)i] * combinedFold;
            captureFin_[(size_t)i] += prevLoopSum_[(size_t)i] * combinedFold;
        }
        // Push foldGain_/glitchFoldGain_ into aloop.dsp's MONITORFOLD/
        // GLITCHFOLD zones (real, live-written zones -- see dsp/aloop.dsp's
        // REGRESSION FOUND AND FIXED comment) so the direct raw-loopSum term
        // there fades out exactly as the native fold fades in. This write
        // MUST happen every block, unconditionally within this g_params
        // branch, or the zone reverts to a dead one exactly like the bug
        // this fixes.
        fui.set("MONITORFOLD", foldGain_);
        fui.set("GLITCHFOLD", glitchFoldGain_);
    }

    // ---- Sampler CAPTURE (recording INTO a sample slot) --------------------
    // Captures from captureFin_ (dry input + folded loop content, NO sampler
    // playback voices) -- deliberately NOT fin_, which by this point ALSO
    // contains this block's own renderInto-mixed sample playback (a genuine
    // self-recording risk a first draft of this fix introduced and caught
    // before it shipped, per audio_thread.cpp's own history). This departs
    // from the original ../looper reference design on purpose (loops are now
    // recordable into sample slots under SHIFT/glitch fold) -- see
    // sampler.h's own top-of-file comment for the exact invariant.
    for (int i = 0; i < N; i++) samplerBuf_[(size_t)i] = (int32_t)(captureFin_[(size_t)i] * 32768.0f);
    sampler->captureBlock(samplerBuf_.data(), N);

    // ---- Run the Faust home stack (loop engine + effects) -------------------
    float* fins[6]  = { fin_.data(), prevFiltOut_.data(), clearBuf_.data(), speedBuf_.data(), masterPhaseBuf_.data(), masterLenBuf_.data() };
    float* fouts[4] = { fout_.data(), rawGlitchTap_.data(), rawLoopSum_.data(), rawFiltTap_.data() };
    faustHome_->compute(N, fins, fouts);

    // Snapshot this block's RAW loop output (rawLoopSum_) for NEXT block's
    // fold-in -- deliberately NOT fout_ (the fx-processed signal), so folded
    // loop content doesn't accumulate a compounding extra pass through the
    // effects chain every block the fold is held.
    prevLoopSum_ = rawLoopSum_;
    // Snapshot this block's fully-effected mix output (rawFiltTap_) for NEXT
    // block's always-effected record fold-in. Deliberately a SEPARATE tap
    // from fout_ (never read fout_ here) even though they're numerically
    // identical -- keeps the live audible path and the record-tap snapshot
    // structurally independent, matching the same discipline as
    // prevLoopSum_ above.
    prevFiltOut_ = rawFiltTap_;

    // Copy the DSP output back out (mono; PluginProcessor duplicates to any
    // additional host output channels).
    float outPeak = 0.0f;
    for (int i = 0; i < N; i++) {
        out[i] = fout_[(size_t)i];
        float a = std::fabs(fout_[(size_t)i]);
        if (a > outPeak) outPeak = a;
    }
    telemetry_.outPeak = outPeak;
}

// ============================================================================
// Ring-buffer export/import for session persistence (getStateInformation/
// setStateInformation). See LooperEngine.h's own extensive derivation comment
// for WHY this specific mechanism is sound; this implementation follows that
// derivation exactly. Not real-time safe by design (many extra compute()
// calls) -- callers must only invoke these from state save/load, never from
// processBlock's hot path.
// ============================================================================

bool LooperEngine::exportLooperRing(int looperIndex, std::vector<int16_t>& outSamples) {
    outSamples.clear();
    if (looperIndex < 0 || looperIndex >= kLooperCount) return false;
    FaustUI& fui = *fui_;
    char z[32];

    // wrapLen: read back via the "finishtarget" zone, which dsp/loop.dsp's
    // wrapLenStep latches to at every finishEdge (see loop.dsp lines 291-320:
    // wrapLenStep(prev) = ba.if(finishEdge, writeIdxForLatch, prev), and
    // writeIdxForLatch prefers finishTargetN whenever finishRequested was
    // true -- which ApcControlSurface always ensures, see its
    // applyRecPlayCycle). A looper that has never been through a finish has
    // finishtarget==0 (its Faust-compiled hslider default) -- nothing to
    // export.
    snprintf(z, sizeof z, "looper%2d/finishtarget", looperIndex);
    long wrapLen = (long)fui.get(z, 0.0f);
    if (wrapLen <= 0) return false;

    // --- Save every zone this export touches, to restore verbatim after ----
    struct SavedLooperZones { float play, vol, rec; };
    std::vector<SavedLooperZones> saved((size_t)kLooperCount);
    for (int lp = 0; lp < kLooperCount; lp++) {
        char zp[32], zv[32], zr[32];
        snprintf(zp, sizeof zp, "looper%2d/play", lp);
        snprintf(zv, sizeof zv, "looper%2d/vol",  lp);
        snprintf(zr, sizeof zr, "looper%2d/rec",  lp);
        saved[(size_t)lp].play = fui.get(zp, 0.0f);
        saved[(size_t)lp].vol  = fui.get(zv, 1.0f);
        saved[(size_t)lp].rec  = fui.get(zr, 0.0f);
    }

    // A looper still actively recording (rec==1) has no finished ring
    // content to export yet -- bail rather than reading an in-progress take
    // (this should not occur at save time in practice, since a DAW session
    // save doesn't interrupt performance gestures, but defensive).
    {
        char zr[32]; snprintf(zr, sizeof zr, "looper%2d/rec", looperIndex);
        if (fui.get(zr, 0.0f) > 0.5f) return false;
    }

    // Force the target's own erase=0 for the export sweep: dsp/loop.dsp's
    // `wipe = max(clearAll, eraseN)` zeroes `hold` (`hold = delayed *
    // (1-recordingGateNow) * (1-wipe)`) whenever eraseN is held, which would
    // silently read back zeros instead of the real ring content if a stale
    // erase gesture happened to still be mid-release (ApcControlSurface's
    // pollHolds always releases it within ~50ms in normal operation, but this
    // is cheap insurance). Restored alongside play/vol below.
    char zerase[32]; snprintf(zerase, sizeof zerase, "looper%2d/erase", looperIndex);
    float savedErase = fui.get(zerase, 0.0f);
    fui.set(zerase, 0.0f);

    // --- Solo the target looper: mute the other 19 (play=0), solo target
    // (play=1, vol=1) so the summed rawLoopSum output reduces to exactly
    // this looper's own `out` signal. Never touches `rec`/`erase`/
    // `finishreq`/`len` for ANY looper -- purely a mix-time gate, see
    // dsp/loop.dsp's `out = loopSig * playN * volN` (playN/volN gate only
    // the OUTPUT term, nothing upstream of it).
    for (int lp = 0; lp < kLooperCount; lp++) {
        char zp[32], zv[32];
        snprintf(zp, sizeof zp, "looper%2d/play", lp);
        snprintf(zv, sizeof zv, "looper%2d/vol",  lp);
        fui.set(zp, (lp == looperIndex) ? 1.0f : 0.0f);
        fui.set(zv, (lp == looperIndex) ? 1.0f : saved[(size_t)lp].vol);
    }

    // --- Sweep masterPhase across a full wrapLen-sample span at effSpeed==
    // 1.0 (so readPos == absPos == wrapAbs(masterPhase - recordStartPhaseOffset,
    // wrapLen) on every sample -- see loop.dsp lines 421-447). Feed silence
    // for in/prevFiltIn/clearAll so no OTHER state changes; masterLen is set
    // to wrapLen purely so the (unrelated, ARM-quantization-only) gridStep
    // calculation stays well-defined -- it cannot trigger armEdge here since
    // armPulse only fires from a genuine recN rising edge (never touched).
    outSamples.resize((size_t)wrapLen);
    long produced = 0;
    double phase = 0.0;   // arbitrary sweep origin -- rotation is immaterial, see the derivation comment
    while (produced < wrapLen) {
        int n = (int)std::min((long)maxBlockSize_, wrapLen - produced);
        for (int i = 0; i < n; i++) {
            fin_[(size_t)i] = 0.0f;
            prevFiltOut_[(size_t)i] = 0.0f;
            clearBuf_[(size_t)i] = 0.0f;
            speedBuf_[(size_t)i] = 1.0f;
            masterLenBuf_[(size_t)i] = (float)wrapLen;
            masterPhaseBuf_[(size_t)i] = (float)std::fmod(phase + (double)i, (double)wrapLen);
        }
        phase = std::fmod(phase + (double)n, (double)wrapLen);

        float* fins[6]  = { fin_.data(), prevFiltOut_.data(), clearBuf_.data(), speedBuf_.data(), masterPhaseBuf_.data(), masterLenBuf_.data() };
        float* fouts[4] = { fout_.data(), rawGlitchTap_.data(), rawLoopSum_.data(), rawFiltTap_.data() };
        faustHome_->compute(n, fins, fouts);

        // rawLoopSum_ (fouts[2]) is the sum of all 20 loopers' `out` -- with
        // the other 19 muted above, this equals exactly looperIndex's own
        // out = (record+hold)*playN*volN, and since rec==0/wipe==0 for the
        // target, record==0 and out==hold==delayed (the pure interpolated
        // ring read) -- the real recorded audio, verbatim.
        for (int i = 0; i < n; i++) {
            float s = rawLoopSum_[(size_t)i];
            int32_t v = (int32_t)std::lround(s * 32768.0f);
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            outSamples[(size_t)(produced + i)] = (int16_t)v;
        }
        produced += n;
    }

    // --- Restore every zone this export touched, verbatim ------------------
    for (int lp = 0; lp < kLooperCount; lp++) {
        char zp[32], zv[32];
        snprintf(zp, sizeof zp, "looper%2d/play", lp);
        snprintf(zv, sizeof zv, "looper%2d/vol",  lp);
        fui.set(zp, saved[(size_t)lp].play);
        fui.set(zv, saved[(size_t)lp].vol);
    }
    fui.set(zerase, savedErase);
    return true;
}

void LooperEngine::importLooperRing(int looperIndex, const std::vector<int16_t>& samples) {
    if (looperIndex < 0 || looperIndex >= kLooperCount) return;
    if (samples.empty()) return;
    FaustUI& fui = *fui_;
    char z[32];

    // Re-record via the SAME write path a live take uses (dsp/loop.dsp's
    // writeVal = prevFiltIn * recordingGateNow * (1-wipe)) -- feed the
    // captured samples as prevFiltIn across N-sample blocks with a real
    // ARM (rec 0->1) already asserted, then FINISH (finishreq pulse +
    // finishtarget = the captured length) so finishEdge fires and
    // wrapLen/recordStartPhaseOffset latch exactly as they would for a
    // genuine recording of this same audio. Mute every OTHER looper's play
    // gate is NOT necessary here (writing is independent of any looper's
    // play/vol -- those only gate the OUTPUT term), so only this looper's
    // own zones are touched.
    snprintf(z, sizeof z, "looper%2d/rec", looperIndex);
    float savedRec = fui.get(z, 0.0f);
    char zplay[32]; snprintf(zplay, sizeof zplay, "looper%2d/play", looperIndex);
    float savedPlay = fui.get(zplay, 0.0f);
    char zft[32]; snprintf(zft, sizeof zft, "looper%2d/finishtarget", looperIndex);
    float savedFinishTarget = fui.get(zft, 0.0f);
    char zfr[32]; snprintf(zfr, sizeof zfr, "looper%2d/finishreq", looperIndex);
    char zerase[32]; snprintf(zerase, sizeof zerase, "looper%2d/erase", looperIndex);
    float savedErase = fui.get(zerase, 0.0f);

    // Force erase=0 for the duration of the import: dsp/loop.dsp's
    // `wipe = max(clearAll, eraseN)` zeroes writeVal whenever eraseN is held
    // (`writeVal = prevFiltIn * recordingGateNow * (1-wipe)`) -- a stale
    // erase=1 left over from a prior gesture would silently discard every
    // sample fed below. ApcControlSurface's own pollHolds() always releases
    // erase back to 0 within ~50ms in normal operation, but this is cheap
    // insurance against reading state mid-release.
    fui.set(zerase, 0.0f);

    // ARM: rec 0->1 (armPulse fires on this rising edge, resetting writeIdx
    // to 0 -- dsp/loop.dsp line ~214). Silence every other input so nothing
    // else about the DSP's state is disturbed.
    fui.set(z, 1.0f);

    const long total = (long)samples.size();
    long fed = 0;
    while (fed < total) {
        int n = (int)std::min((long)maxBlockSize_, total - fed);
        for (int i = 0; i < n; i++) {
            fin_[(size_t)i] = 0.0f;
            prevFiltOut_[(size_t)i] = (float)samples[(size_t)(fed + i)] / 32768.0f;
            clearBuf_[(size_t)i] = 0.0f;
            speedBuf_[(size_t)i] = 1.0f;
            masterLenBuf_[(size_t)i] = (float)total;
            // masterPhase during import is irrelevant to the WRITE path
            // (writeIdx/writeVal do not depend on masterPhase at all -- only
            // the READ side does), so hold it at 0 throughout; the read side
            // is inaudible during this looper's own recording anyway
            // (recordingGateNow gates hold to 0 while recording -- dsp/loop.dsp
            // line 489), and every OTHER looper's masterPhase-driven read
            // continuing at 0 here is fine since this whole import runs
            // between processBlock calls, not interleaved with live playback.
            //
            // PHASE-LOCK CONSISTENCY ACROSS RESTORED LOOPERS: holding
            // masterPhase at exactly 0.0 for EVERY looper's import (not just
            // this one) means recordStartPhaseOffsetStep's finishEdge latch
            // (dsp/loop.dsp line 421: ba.if(finishEdge, masterPhase, prev))
            // sets recordStartPhaseOffset=0 uniformly across every looper
            // setStateInformation restores this way -- so all restored
            // loopers anchor to the SAME reference point and stay mutually
            // in-phase with each other after reload (absPos = wrapAbs(
            // masterPhase - recordStartPhaseOffset, wrapLen) reduces to
            // wrapAbs(masterPhase, wrapLen) for all of them), exactly as if
            // they had all been freshly recorded back-to-back at the same
            // instant. Using the LIVE running masterPhase instead here would
            // have been wrong: it would anchor each looper to whatever
            // instant its own importLooperRing call happened to run at
            // (loopers are restored one at a time in a loop in
            // setStateInformation), reintroducing exactly the
            // inter-looper drift dsp/loop.dsp's masterPhase design exists to
            // prevent -- see loop.dsp's own "PHRASE-LOCK" comment.
            masterPhaseBuf_[(size_t)i] = 0.0f;
        }
        float* fins[6]  = { fin_.data(), prevFiltOut_.data(), clearBuf_.data(), speedBuf_.data(), masterPhaseBuf_.data(), masterLenBuf_.data() };
        float* fouts[4] = { fout_.data(), rawGlitchTap_.data(), rawLoopSum_.data(), rawFiltTap_.data() };
        faustHome_->compute(n, fins, fouts);
        fed += n;
    }

    // FINISH: release rec FIRST (matching ApcControlSurface::applyRecPlayCycle's
    // real press-time ordering: `setLooper(ps, looper, "rec", 0.0f)` happens
    // BEFORE the finishtarget/finishreq push), THEN push the target (the
    // exact captured length -- no quantization ambiguity here, we know
    // precisely how many samples to restore) and pulse finishreq.
    //
    // WHY THE ORDER MATTERS (a real bug caught while tracing this, not a
    // stylistic preference): `recordingGate(prev) = (recN>0.5) |
    // (finishRequested & (prev<finishTargetN))` -- if rec were released
    // AFTER pushing finishtarget/finishreq (as an earlier draft of this
    // function did), recN would still read >0.5 during the compute() call
    // that applies the finish push, so recordingGate stays true from the
    // recN term ALONE regardless of finishTargetN/finishRequested, and
    // writeIdx keeps incrementing PAST `total` for that block -- corrupting
    // the exact sample count this function just spent the whole feed loop
    // producing. Releasing rec first (recN=0) means recordingGate's first
    // term goes false immediately, and finishRequested/finishTargetN (not
    // yet pushed) also false, so recordingGate reads false and writeIdx
    // correctly HOLDS at exactly `total` for the one compute() call in
    // between -- then pushing finishtarget=total/finishreq=1 the very next
    // call makes finishRequested true with prev(writeIdx)==total, so
    // `prev<finishTargetN` is false too (total is not < total), keeping
    // recordingGate false and writeIdx still holding at `total` -- finishEdge
    // fires from recordingGateNow's true->false transition that already
    // happened at the rec-release block, landing wrapLen=total exactly.
    fui.set(z, 0.0f);
    {
        int n = std::min(8, maxBlockSize_);
        for (int i = 0; i < n; i++) {
            fin_[(size_t)i] = 0.0f;
            prevFiltOut_[(size_t)i] = 0.0f;
            clearBuf_[(size_t)i] = 0.0f;
            speedBuf_[(size_t)i] = 1.0f;
            masterLenBuf_[(size_t)i] = (float)total;
            masterPhaseBuf_[(size_t)i] = 0.0f;
        }
        float* fins[6]  = { fin_.data(), prevFiltOut_.data(), clearBuf_.data(), speedBuf_.data(), masterPhaseBuf_.data(), masterLenBuf_.data() };
        float* fouts[4] = { fout_.data(), rawGlitchTap_.data(), rawLoopSum_.data(), rawFiltTap_.data() };
        faustHome_->compute(n, fins, fouts);
    }
    // Now push finishtarget/finishreq (finishRequested latches true on this
    // next compute() call; recordingGateNow was already false since the
    // rec-release block above, so writeIdx has been correctly holding at
    // `total` for one full block already -- this push just makes
    // wrapLenStep's ba.if(finishEdge, writeIdxForLatch, prev) condition see
    // finishRequested=true for the NEXT finishEdge check, but finishEdge
    // itself already fired at the rec-release transition above, so wrapLen
    // has ALREADY latched to `total` by this point). This push exists to
    // leave finishtarget/finishreq at the same steady-state values a real
    // ApcControlSurface-driven finish would leave them at (finishtarget=
    // total, matching what exportLooperRing reads back on a future save),
    // not to re-trigger a second finishEdge.
    fui.set(zft, (float)total);
    fui.set(zfr, 1.0f);
    {
        int n = std::min(8, maxBlockSize_);
        for (int i = 0; i < n; i++) {
            fin_[(size_t)i] = 0.0f;
            prevFiltOut_[(size_t)i] = 0.0f;
            clearBuf_[(size_t)i] = 0.0f;
            speedBuf_[(size_t)i] = 1.0f;
            masterLenBuf_[(size_t)i] = (float)total;
            masterPhaseBuf_[(size_t)i] = 0.0f;
        }
        float* fins[6]  = { fin_.data(), prevFiltOut_.data(), clearBuf_.data(), speedBuf_.data(), masterPhaseBuf_.data(), masterLenBuf_.data() };
        float* fouts[4] = { fout_.data(), rawGlitchTap_.data(), rawLoopSum_.data(), rawFiltTap_.data() };
        faustHome_->compute(n, fins, fouts);
    }
    fui.set(zfr, 0.0f);   // release the momentary finishreq pulse, matching ApcControlSurface's pollHolds release pattern

    // Restore play/vol to whatever the caller (setStateInformation) will set
    // them to right after this call -- rec/finishtarget/finishreq are left
    // at their natural post-FINISH values (rec=0, finishreq=0, finishtarget=
    // total), which IS the correct final state for a looper that just
    // finished recording `total` samples, so nothing further to restore
    // there. play is restored to its pre-import value here defensively (the
    // caller sets it explicitly afterward anyway per the saved session
    // state, so this is belt-and-suspenders, not load-bearing).
    fui.set(zplay, savedPlay);
    fui.set(zerase, savedErase);
    (void)savedRec; (void)savedFinishTarget;
}

} // namespace aloopvst
