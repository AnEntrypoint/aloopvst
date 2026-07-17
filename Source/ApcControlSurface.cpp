// ApcControlSurface — ported from aloop's src/control/apc_grid.cpp (the
// ApcGrid state machine) and the note/CC dispatch table inside
// src/control/midi.cpp's runMidiLoop channel-0/channel-1 handling. See
// ApcControlSurface.h for what's genuinely different (host-transport instead
// of Link, block-rate polling instead of a separate ALSA thread, no LED
// output). Every comment carried over from the original documents load-bearing
// behavior this port must not silently change.
#include "ApcControlSurface.h"
#include <cstdlib>

namespace aloopvst {

void ApcControlSurface::bindAll(ParamStore& ps) {
    char name[32];
    for (int looper = 0; looper < kLooperCount; looper++) {
        for (const char* field : {"rec", "play", "erase", "finishreq"}) {
            snprintf(name, sizeof name, "looper%d/%s", looper, field);
            ps.bind(name);
        }
        // "len" bound separately with dsp/loop.dsp's actual hslider default
        // (48000 = 1 second at 48kHz) -- matches the zero-default bug class
        // ParamStore::bind's own doc comment warns about.
        snprintf(name, sizeof name, "looper%d/len", looper);
        ps.bind(name, 48000.0f);
        snprintf(name, sizeof name, "looper%d/finishtarget", looper);
        ps.bind(name, 0.0f);
    }
    ps.bind("fx/pitchbend");
    ps.bind("fx/pitchbend_engaged");
    ps.bind("fx/microrepeat_div");
    ps.bind("fx/monitorfold");
    ps.bind("fx/formant");
    // fx/* runtime knobs -- seeded with dsp/effects_runtime.dsp's own
    // compiled-in defaults (see midi.cpp's kFxDefaults table), since
    // PluginProcessor's AudioProcessorValueTreeState parameters write into
    // these same ParamStore targets and must not silently force a non-zero-
    // default zone to 0.0 before the user ever touches a knob.
    ps.bind("fx/hp",      0.0f);   // HPCUT   0.0 = bypass
    ps.bind("fx/lpres",   0.0f);   // LPRES   0.0 = no resonance
    ps.bind("fx/lp",      1.0f);   // LPCUT   1.0 = fully open (bypass)
    ps.bind("fx/reverb",  0.0f);   // REVAMT  0.0 = dry
    ps.bind("fx/delay",   0.0f);   // DELAYAMT 0.0 = dry
    ps.bind("fx/time",    0.5f);   // TIME    0.5 = the Faust default
    ps.bind("fx/pitch",   0.0f);   // SEMIS   0.0 = unity
    ps.bind("cmd/master_len", 0.0f);
    ps.bind("cmd/recorded_bpm", 0.0f);
    ps.bind("cmd/clearall", 0.0f);
    ps.bind("cmd/halfspeed", 0.0f);
    ps.bind("cmd/doublespeed", 0.0f);
    ps.bind("cmd/stopall", 0.0f);
}

static void setLooper(ParamStore& ps, int looper, const char* field, float v) {
    char name[32];
    snprintf(name, sizeof name, "looper%d/%s", looper, field);
    ps.setByName(name, v);
}

// Derives the shared phrase from the FIRST loop's raw recorded length only
// (never re-derived from later loopers -- see ApcControlSurface.h's own
// note that all subsequent loopers quantize AGAINST this single detected
// phrase, they never redefine it). Rewritten per explicit spec (replacing
// the old fixed-4-bar-base/absolute-beat-count scheme, which searched a
// hardcoded {1,2,4,8,...,128} beat-count set against an assumed 4-bar
// quantum -- a different, more convoluted scheme than what's actually
// wanted here): treat the raw recorded length as implying some tempo
// (recordedSeconds = 1 beat), then try every power-of-2 SCALING of that
// same length (both stretching and compressing it) and keep whichever
// scaling's implied BPM lands closest to 120 -- exact math, no beat-count
// table, no [80,160]-window special case. A minimum of a 16th note (1/4 of
// one beat) at the CHOSEN tempo is enforced as a floor, so a short take
// (e.g. one hi-hat hit) can resolve to a fast 16th-note-length phrase
// instead of being force-stretched up to a whole beat or more.
struct TempoSolveResult {
    double bpm;
    double beats;       // the winning scaling, in BEATS (may be < 1, e.g. 0.25 for a 16th note)
};
static TempoSolveResult deriveTempoQuant(double seconds) {
    if (seconds <= 0.0) return {120.0, 1.0};
    const double kSixteenthBeats = 0.25;   // 1/4 of a beat = a 16th note
    // Model: the recorded length spans `beats` beats at the chosen tempo,
    // so bpm = 60*beats/seconds -- INCREASING in beats (more beats packed
    // into the same fixed recorded length means each beat is shorter, i.e.
    // faster). `beats` is constrained to a power of 2 (2^k for integer k,
    // including negative k for sub-beat subdivisions like a 16th note).
    // The exact (non-power-of-2) beats value that would land bpm at
    // precisely 120 is `2*seconds` (solving 60*beats/seconds=120 for
    // beats); the nearest achievable power-of-2 is whichever integer k is
    // closest to log2 of that (evaluate a small neighborhood around the
    // floor, since rounding log2 naively can pick the wrong side when the
    // true optimum sits close to a .5 boundary).
    //
    // ROOT CAUSE of a real bug caught in testing (a 60ms recording
    // resolved to a 3.4-SECOND phrase -- 56x longer than what was
    // recorded, exactly backwards from the intended "short takes become
    // fast subdivisions" behavior): an earlier draft of this function used
    // bpm = rawBpm/beats (rawBpm = 60/seconds, i.e. treating the raw
    // length as exactly 1 beat and dividing by a scaling factor) -- that
    // formula has beats and bpm move in the WRONG relative direction, so
    // the search picked large beats values to LOWER an already-too-fast
    // rawBpm instead of picking small (sub-1) beats values to describe a
    // short recording as a fast subdivision. Verified against the correct
    // model above via manual calculation before fixing.
    double exactBeats = 2.0 * seconds;
    double kExact = std::log2(exactBeats);
    long kFloor = (long)std::floor(kExact);
    double bestDist = 1e18;
    TempoSolveResult best = {120.0, 1.0};
    bool found = false;
    for (long k = kFloor - 1; k <= kFloor + 2; k++) {
        double beats = std::pow(2.0, (double)k);
        if (beats < kSixteenthBeats) continue;   // never propose below a 16th note
        double bpm = 60.0 * beats / seconds;
        double dist = std::fabs(bpm - 120.0);
        if (dist < bestDist) { bestDist = dist; best = {bpm, beats}; found = true; }
    }
    // Degenerate case: even the 16th-note floor produces a beats value
    // this loop's neighborhood search missed (e.g. an extremely short
    // recording where kFloor-1 also falls below the floor) -- fall back to
    // the floor itself directly rather than the arbitrary {120,1.0}
    // default, so a very short take still resolves to SOMETHING sane.
    if (!found) {
        double bpm = 60.0 * kSixteenthBeats / seconds;
        best = {bpm, kSixteenthBeats};
    }
    return best;
}

void ApcControlSurface::applyRecPlayCycle(int looper, unsigned now_ms, ParamStore& ps, LooperEngine* engine) {
    // Real 3-state cycle (ported verbatim from apc_grid.cpp's
    // applyRecPlayCycle): empty -> ARM (rec=1, held for the whole recording
    // pass) -> FINISH (rec=0, play=1) -> pause (play=0) -> resume (play=1).
    const long sr = (long)sampleRate_;
    if (m_looperRecording[looper]) {
        setLooper(ps, looper, "rec", 0.0f);   // FINISH: stop recording
        m_looperRecording[looper] = false;
        m_looperHasContent[looper] = true;
        m_looperPlaying[looper] = true;
        setLooper(ps, looper, "play", 1.0f);

        // Re-sync from the shared store first (processBlock resets
        // cmd/master_len to 0 on CLEAR_ALL) -- keeps both in agreement.
        m_masterLenSamples = (long)ps.get("cmd/master_len", 0.0f);
        if (m_masterLenSamples == 0) {
            // EXACT elapsed sample count, read LIVE from the writeidx Faust
            // zone via fui() (dsp/loop.dsp counts writeIdx up unconditionally
            // while recordingGate is true, so its value at this exact
            // instant -- BEFORE the finish push below changes anything -- is
            // precisely how many samples this take wrote, no estimate
            // needed). Deliberately NOT engine->snapshotTelemetry(): that
            // snapshot is only refreshed once per LooperEngine::processBlock
            // call, so by the time a press dispatched mid-block reaches here,
            // it reflects the PREVIOUS block's writeIdx, not the true
            // current value -- a real, confirmed one-block-stale read (this
            // exact staleness produced a measurably wrong recorded_bpm in
            // testing). fui().get() reads the live zone directly, with no
            // such lag. Previously this also used a wall-clock millisecond
            // estimate (now_ms - m_recordStartMs[looper]) with a GUESSED
            // 10ms "safety margin" subtracted to compensate for that
            // estimate's own imprecision -- a workaround for not having the
            // real number, not a correct measurement; reading the live zone
            // makes that margin unnecessary too. Falls back to the wall-clock
            // estimate only if engine is unavailable (defensive; should not
            // happen once prepareToPlay has run), matching the same fallback
            // the subsequent-recording branch below already uses.
            long lenSamples;
            if (engine) {
                char z[32]; snprintf(z, sizeof z, "looper%2d/writeidx", looper);
                lenSamples = (long)engine->fui().get(z, 0.0f);
            } else {
                unsigned elapsedMs = now_ms - m_recordStartMs[looper];
                lenSamples = (long)elapsedMs * sr / 1000;
            }
            if (lenSamples < 64) lenSamples = 64;
            if (lenSamples > kMaxLoopSamples_) lenSamples = kMaxLoopSamples_;
            m_masterLenSamples = lenSamples;
            ps.setByName("cmd/master_len", (float)m_masterLenSamples);

            // TRUE varispeed: lock in the BPM this shared phrase was
            // established at, via the beat-count solver, so
            // m_masterLenSamples snaps to a clean, grid-aligned value.
            double recordedSeconds = (double)m_masterLenSamples / (double)sr;
            TempoSolveResult solved = deriveTempoQuant(recordedSeconds);
            long quantizedLenSamples = (long)(solved.beats * 60.0 / solved.bpm * sr + 0.5);
            if (quantizedLenSamples < 64) quantizedLenSamples = 64;
            if (quantizedLenSamples > kMaxLoopSamples_) quantizedLenSamples = kMaxLoopSamples_;
            m_masterLenSamples = quantizedLenSamples;
            ps.setByName("cmd/master_len", (float)m_masterLenSamples);
            ps.setByName("cmd/recorded_bpm", (float)solved.bpm);

            for (int lp = 0; lp < kLooperCount; lp++) setLooper(ps, lp, "len", (float)m_masterLenSamples);

            // NOTE: the original's two-way Link tempo proposal
            // (link->proposeTempo(solved.bpm)) has no VST equivalent -- a
            // plugin cannot set its host's tempo. This is a deliberate,
            // documented behavior drop (not a silent omission): solved.bpm
            // is still computed and stored (cmd/recorded_bpm) for the
            // varispeed ratio math below, it's just never pushed anywhere
            // external.

            // FINISH-QUANTIZATION: push the target and pulse finishreq so
            // the DSP extends/trims to it sample-accurately.
            setLooper(ps, looper, "finishtarget", (float)m_masterLenSamples);
            setLooper(ps, looper, "finishreq", 1.0f);
            m_looperFinishReqReleaseAt[looper] = now_ms + 50;
        } else {
            // Subsequent recording: snap this looper's own elapsed record
            // duration to the nearest musical subdivision/multiple of the
            // established phrase, via an UNBOUNDED geometric candidate
            // sequence (powers of 2) and the confirmed 68% bracket
            // threshold (extend to the upper candidate only if raw is past
            // 68% of the way from lower to upper; else trim to lower) --
            // ported verbatim from apc_grid.cpp, see that file's own
            // WITNESSED commentary for the two rounds of bugs this fixes
            // (M/16 minimum, and the 68% threshold replacing a
            // fixed-small-set nearest-distance search).
            //
            // ARM-QUANTIZATION compensation: read writeIdx's own zone LIVE
            // via fui() (the TRUE elapsed sample count since the real,
            // grid-aligned arm instant), not via LooperEngine's
            // snapshotTelemetry() -- that snapshot only refreshes once per
            // processBlock call, so a press dispatched mid-block would read
            // the PREVIOUS block's writeIdx, a real one-block-stale value
            // (confirmed: this exact staleness produced a measurably wrong
            // quantized length in testing of the first-recording branch
            // above, which had the identical bug). Falls back to the
            // wall-clock estimate only if the engine pointer is unavailable
            // (defensive; should not happen once prepareToPlay has run).
            long rawSamples;
            if (engine) {
                char z[32]; snprintf(z, sizeof z, "looper%2d/writeidx", looper);
                rawSamples = (long)engine->fui().get(z, 0.0f);
            } else {
                unsigned elapsedMs = now_ms - m_recordStartMs[looper];
                rawSamples = (long)elapsedMs * sr / 1000;
            }
            double ratio = (double)rawSamples / (double)m_masterLenSamples;
            if (ratio < 1.0 / 16.0) ratio = 1.0 / 16.0;   // floor: never propose below M/16
            double log2Ratio = std::log2(ratio);
            double lowerExp = std::floor(log2Ratio);
            double lowerCand = (double)m_masterLenSamples * std::pow(2.0, lowerExp);
            double upperCand = (double)m_masterLenSamples * std::pow(2.0, lowerExp + 1.0);
            if (upperCand > (double)kMaxLoopSamples_) upperCand = (double)kMaxLoopSamples_;
            if (lowerCand > upperCand) lowerCand = upperCand;   // degenerate guard at the ceiling
            double span = upperCand - lowerCand;
            double bestLen;
            if (span <= 0.0) {
                bestLen = lowerCand;
            } else {
                double frac = ((double)rawSamples - lowerCand) / span;
                bestLen = (frac >= 0.68) ? upperCand : lowerCand;
            }
            long quantized = (long)(bestLen + 0.5);
            if (quantized < 64) quantized = 64;
            if (quantized > kMaxLoopSamples_) quantized = kMaxLoopSamples_;

            setLooper(ps, looper, "finishtarget", (float)quantized);
            setLooper(ps, looper, "finishreq", 1.0f);
            m_looperFinishReqReleaseAt[looper] = now_ms + 50;
        }
    } else if (!m_looperHasContent[looper]) {
        setLooper(ps, looper, "rec", 1.0f);   // ARM: start recording
        m_looperRecording[looper] = true;
        m_recordStartMs[looper] = now_ms;
    } else if (m_looperPlaying[looper]) {
        setLooper(ps, looper, "play", 0.0f);
        m_looperPlaying[looper] = false;
    } else {
        setLooper(ps, looper, "play", 1.0f);
        m_looperPlaying[looper] = true;
    }
}

void ApcControlSurface::forgetLooperFromPresets(int looper) {
    uint32_t bit = (1u << looper);
    for (int p = 0; p < kPresetCount; p++) {
        if (!m_presetUsed[p]) continue;
        if (!(m_presetMask[p] & bit)) continue;
        m_presetMask[p] &= ~bit;
        if (m_presetMask[p] == 0) m_presetUsed[p] = false;
    }
}

void ApcControlSurface::onPadPress(int note, unsigned now_ms, ParamStore& ps, LooperEngine* engine) {
    int row = note / kApcCols, col = note % kApcCols;

    int looper = gridLooperIndex(row, col);
    if (looper >= 0) {
        // REPEATED-NOTE-ON GUARD: some controllers re-send note-on for a pad
        // that is physically still held down; without this guard a repeat
        // arriving mid-recording would re-fire applyRecPlayCycle and
        // prematurely finish the take (see apc_grid.cpp's own WITNESSED
        // note). Only treat this as a genuinely NEW press when the pad was
        // not already marked held.
        bool alreadyHeld = m_looperHeld[looper];
        m_looperHeld[looper] = true;
        if (alreadyHeld) return;

        m_looperErased[looper] = false;
        m_looperHoldStart[looper] = now_ms;
        // ALL transitions fire on PRESS now (arm, finish, pause, resume) --
        // simplified from the original split (arm/finish on press,
        // pause/resume on release) per explicit user request: every tap of
        // a pad is one discrete gesture, so the whole rec/play/pause/resume
        // cycle advances on the press instant alone, with no state change
        // left pending for the matching release. onPadRelease below still
        // only clears the held-flag bookkeeping (needed for the repeated-
        // note-on guard and the hold-to-erase timer), it never advances the
        // cycle itself anymore.
        applyRecPlayCycle(looper, now_ms, ps, engine);
        return;
    }
    int preset = gridPresetIndex(row, col);
    if (preset >= 0) {
        m_presetHeld[preset] = true;
        m_presetCaptured[preset] = false;
        m_presetHoldStart[preset] = now_ms;
        return;
    }
}

void ApcControlSurface::onPadRelease(int note, unsigned now_ms, ParamStore& ps, LooperEngine* engine) {
    int row = note / kApcCols, col = note % kApcCols;
    (void)ps; (void)engine;   // no longer used here -- the cycle itself advances on press now

    int looper = gridLooperIndex(row, col);
    if (looper >= 0) {
        // The rec/play/pause/resume cycle now advances entirely on press
        // (see onPadPress) -- release only clears the held-flag so the
        // repeated-note-on guard and the hold-to-erase timer (pollHolds)
        // both see the pad as no longer down.
        m_looperHeld[looper] = false;
        return;
    }
    int preset = gridPresetIndex(row, col);
    if (preset >= 0) {
        if (m_presetHeld[preset] && !m_presetCaptured[preset]) {
            if (m_presetUsed[preset]) applyPreset(preset, ps);
        }
        m_presetHeld[preset] = false;
        return;
    }
}

void ApcControlSurface::pollHolds(unsigned now_ms, ParamStore& ps) {
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (m_looperEraseReleaseAt[looper] != 0 && now_ms >= m_looperEraseReleaseAt[looper]) {
            setLooper(ps, looper, "erase", 0.0f);
            m_looperEraseReleaseAt[looper] = 0;
        }
    }
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (m_looperFinishReqReleaseAt[looper] != 0 && now_ms >= m_looperFinishReqReleaseAt[looper]) {
            setLooper(ps, looper, "finishreq", 0.0f);
            m_looperFinishReqReleaseAt[looper] = 0;
        }
    }
    for (int looper = 0; looper < kLooperCount; looper++) {
        if (!m_looperHeld[looper] || m_looperErased[looper]) continue;
        if (now_ms - m_looperHoldStart[looper] < kHoldEraseMs) continue;
        // Long-hold -> erase. Release erase back to 0 after a short delay
        // (not immediately in the same call) -- setting it to 1 then
        // immediately back to 0 would race the DSP read with no ordering
        // guarantee. See apc_grid.cpp's own WITNESSED note on this exact bug
        // (erase stuck at 1 forever silently wiping playback every block).
        setLooper(ps, looper, "erase", 1.0f);
        m_looperEraseReleaseAt[looper] = now_ms + 50;
        if (m_looperRecording[looper]) {
            setLooper(ps, looper, "rec", 0.0f);
            m_looperRecording[looper] = false;
        }
        m_looperErased[looper] = true;
        m_looperHasContent[looper] = false;
        m_looperPlaying[looper] = false;
        setLooper(ps, looper, "play", 0.0f);
        forgetLooperFromPresets(looper);
    }
    // Once erasing this way leaves NO looper with content anywhere in the
    // rig, the master phrase is genuinely gone -- reset so the next
    // recording re-establishes a fresh phrase from scratch (mirrors
    // onClearAll's own reset).
    bool anyHasContent = false;
    for (int lp = 0; lp < kLooperCount; lp++) if (m_looperHasContent[lp]) { anyHasContent = true; break; }
    if (!anyHasContent && m_masterLenSamples != 0) {
        m_masterLenSamples = 0;
        ps.setByName("cmd/master_len", 0.0f);
        ps.setByName("cmd/recorded_bpm", 0.0f);
    }
    for (int p = 0; p < kPresetCount; p++) {
        if (!m_presetHeld[p] || m_presetCaptured[p]) continue;
        if (now_ms - m_presetHoldStart[p] < kHoldEraseMs) continue;
        capturePreset(p, ps);
        m_presetCaptured[p] = true;
    }
}

void ApcControlSurface::capturePreset(int p, ParamStore& /*ps*/) {
    if (p < 0 || p >= kPresetCount) return;
    uint32_t mask = 0;
    for (int n = 0; n < kLooperCount; n++)
        if (m_looperHasContent[n] && m_looperPlaying[n]) mask |= (1u << n);
    m_presetMask[p] = mask;
    m_presetUsed[p] = true;
}

void ApcControlSurface::applyPreset(int p, ParamStore& ps) {
    if (p < 0 || p >= kPresetCount || !m_presetUsed[p]) return;
    uint32_t mask = m_presetMask[p];
    for (int n = 0; n < kLooperCount; n++) {
        if (!m_looperHasContent[n]) continue;
        bool shouldPlay = (mask & (1u << n)) != 0;
        if (shouldPlay != m_looperPlaying[n]) {
            setLooper(ps, n, "play", shouldPlay ? 1.0f : 0.0f);
            m_looperPlaying[n] = shouldPlay;
        }
    }
}

// --- live pitch (CC1 mod-wheel deadzone, CC52 absolute) ---------------------
void ApcControlSurface::onModWheel(uint8_t data2, ParamStore& ps) {
    if (!m_liveEngaged) { ps.setByName("fx/pitchbend_engaged", 0.0f); ps.setByName("fx/pitchbend", 0.0f); return; }
    bool inDeadzone = (data2 >= 59 && data2 <= 69);
    if (inDeadzone) {
        ps.setByName("fx/pitchbend_engaged", 0.0f);
        ps.setByName("fx/pitchbend", 0.0f);
    } else {
        float semis = ((float)((int)data2 - 64)) * 12.0f / 63.0f;
        ps.setByName("fx/pitchbend", semis);
        ps.setByName("fx/pitchbend_engaged", 1.0f);
    }
}
void ApcControlSurface::onAbsolutePitch(uint8_t data2, ParamStore& ps) {
    if (!m_liveEngaged) { ps.setByName("fx/pitchbend_engaged", 0.0f); ps.setByName("fx/pitchbend", 0.0f); return; }
    float semis = (data2 / 127.0f) * 24.0f - 12.0f;
    ps.setByName("fx/pitchbend", semis);
    ps.setByName("fx/pitchbend_engaged", 1.0f);
}
void ApcControlSurface::onLiveEngageToggle(ParamStore& ps) {
    m_liveEngaged = !m_liveEngaged;
    if (!m_liveEngaged) {
        ps.setByName("fx/pitchbend", 0.0f);
        ps.setByName("fx/pitchbend_engaged", 0.0f);
    }
}
void ApcControlSurface::onStopImmediate(ParamStore& ps) {
    // SHIFT+STOP_ALL: stop ALL playback AND abort any in-progress recording
    // (unshifted STOP_ALL only stops playback).
    for (int lp = 0; lp < kLooperCount; lp++) {
        if (m_looperRecording[lp]) {
            setLooper(ps, lp, "rec", 0.0f);
            m_looperRecording[lp] = false;
        }
        setLooper(ps, lp, "play", 0.0f);
        m_looperPlaying[lp] = false;
    }
}
void ApcControlSurface::onClearAll(bool held, ParamStore& ps) {
    ps.setByName("cmd/clearall", held ? 1.0f : 0.0f);
    if (!held) return;
    for (int lp = 0; lp < kLooperCount; lp++) {
        m_looperHeld[lp] = false;
        m_looperErased[lp] = false;
        m_looperPlaying[lp] = false;
        m_looperHasContent[lp] = false;
        m_looperRecording[lp] = false;
        m_recordStartMs[lp] = 0;
        // Explicitly stop every looper's play AND rec Faust gate (not just
        // the C++ shadow) -- see apc_grid.cpp's own two WITNESSED notes on
        // this exact class of bug (a stuck play=1 or rec=1 zone silently
        // surviving a clear).
        setLooper(ps, lp, "play", 0.0f);
        setLooper(ps, lp, "rec", 0.0f);
        setLooper(ps, lp, "finishreq", 0.0f);
        m_looperFinishReqReleaseAt[lp] = 0;
    }
    for (int p = 0; p < kPresetCount; p++) {
        m_presetHeld[p] = false;
        m_presetCaptured[p] = false;
        m_presetUsed[p] = false;
        m_presetMask[p] = 0;
    }
    m_masterLenSamples = 0;
    ps.setByName("cmd/master_len", 0.0f);
    ps.setByName("cmd/recorded_bpm", 0.0f);
}
void ApcControlSurface::onKeybedNoteOn(int note, ParamStore& ps, Sampler* sampler) {
    // The sampler takes the keys when it has content. In drum-record mode
    // (button 66 held) a key press records into THAT key's own drum slot.
    // Otherwise, if a chromatic sample is loaded OR this key has its own
    // drum slot, the key triggers sampler playback. With no sampler content,
    // the key falls through to live-pitch.
    if (sampler) {
        int keyIdx = Sampler::keyIndex(note);
        if (m_drumRecordMode) {
            if (keyIdx >= 0) sampler->pushEvent(Sampler::EV_REC_START, keyIdx, 0);
            return;
        }
        if (sampler->chromaticLoaded() || sampler->drumLoaded(keyIdx)) {
            sampler->pushEvent(Sampler::EV_NOTE_ON, note, 127);
            return;
        }
    }
    // Any keybed key press engages live-pitch at that key's own semitone
    // offset from middle-C-ish (note 60), unconditionally.
    m_liveEngaged = true;
    float semis = (float)(note - 60);
    ps.setByName("fx/pitchbend", semis);
    ps.setByName("fx/pitchbend_engaged", 1.0f);
}
void ApcControlSurface::onKeybedNoteOff(int note, Sampler* sampler) {
    // The sampler NOTE_OFF is forwarded UNCONDITIONALLY (not gated on
    // chromaticLoaded/drumLoaded) -- gating it could strand a sustaining
    // voice if content changed between press and release.
    if (!sampler) return;
    if (m_drumRecordMode) {
        int keyIdx = Sampler::keyIndex(note);
        if (keyIdx >= 0) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
        return;
    }
    sampler->pushEvent(Sampler::EV_NOTE_OFF, note, 0);
}
void ApcControlSurface::onSamplerBtn65Press(Sampler* sampler) {
    if (sampler) sampler->pushEvent(Sampler::EV_REC_START, -1, 0);
}
void ApcControlSurface::onSamplerBtn65Release(Sampler* sampler) {
    if (sampler) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
}
void ApcControlSurface::onSamplerBtn66Press() {
    m_drumRecordMode = true;
}
void ApcControlSurface::onSamplerBtn66Release(Sampler* sampler) {
    m_drumRecordMode = false;
    if (sampler) sampler->pushEvent(Sampler::EV_REC_STOP, 0, 0);
}

// --- microrepeat latch (notes 82-86) ----------------------------------------
void ApcControlSurface::onMicrorepeatOn(int note, ParamStore& ps) {
    static const uint8_t div[5] = {1, 2, 4, 8, 16};
    if (note < 82 || note > 86) return;
    m_microRepeatDiv = div[note - 82];
    ps.setByName("fx/microrepeat_div", (float)m_microRepeatDiv);
}
void ApcControlSurface::onMicrorepeatOff(int note, ParamStore& ps) {
    static const uint8_t div[5] = {1, 2, 4, 8, 16};
    if (note < 82 || note > 86) return;
    if (m_microRepeatDiv == div[note - 82]) {
        m_microRepeatDiv = 0;
        ps.setByName("fx/microrepeat_div", 0.0f);
    }
}

// --- SHIFT -------------------------------------------------------------------
void ApcControlSurface::onShiftPress(ParamStore& ps) {
    m_shift = true;
    ps.setByName("fx/monitorfold", 1.0f);
}
void ApcControlSurface::onShiftRelease(ParamStore& ps) {
    m_shift = false;
    ps.setByName("fx/monitorfold", 0.0f);
}

// --- CC53 formant depth ------------------------------------------------------
void ApcControlSurface::onFormantCC(uint8_t data2, ParamStore& ps) {
    const bool inDeadzone = (data2 >= 60 && data2 <= 68);
    if (inDeadzone) { ps.setByName("fx/formant", 0.0f); return; }
    const float range = m_shift ? 3.0f : 1.0f;
    float v = (((float)(int)data2 - 64.0f) / 63.0f) * range;
    if (v > 3.0f) v = 3.0f; else if (v < -3.0f) v = -3.0f;
    ps.setByName("fx/formant", v);
}

// --- dispatch: the plugin-side equivalent of midi.cpp's channel-0/channel-1
// note/CC routing table, now driven from a decoded MidiEvt instead of raw
// rawmidi bytes. Every note/CC number and the exact ordering of checks is
// preserved verbatim from midi.cpp's runMidiLoop.
void ApcControlSurface::dispatchEvent(const MidiEvt& ev, unsigned nowMs, ParamStore& ps, LooperEngine* engine) {
    int type = ev.type, channel = ev.channel, d1 = ev.d1, d2 = ev.d2;

    if (channel == 0) {
        // SHIFT (channel-0-only guard is this whole `if (channel==0)` block
        // itself -- the keybed's channel-1 note 98 is a different physical
        // key and must never be treated as SHIFT).
        if (d1 == kApcBtnShift) {
            if (type == 0x90 && d2 > 0) { onShiftPress(ps); return; }
            if (type == 0x80 || (type == 0x90 && d2 == 0)) { onShiftRelease(ps); return; }
        }
        if (d1 == kApcLiveLedNote && type == 0x90 && d2 > 0) { onLiveEngageToggle(ps); return; }
        if (type == 0xB0 && d1 == 1)  { onModWheel((uint8_t)d2, ps); return; }
        if (type == 0xB0 && d1 == 52) { onAbsolutePitch((uint8_t)d2, ps); return; }
        if (type == 0xB0 && d1 == 53) { onFormantCC((uint8_t)d2, ps); return; }
        if (d1 >= 82 && d1 <= 86) {
            if (type == 0x90 && d2 > 0) { onMicrorepeatOn(d1, ps); return; }
            if (type == 0x80 || (type == 0x90 && d2 == 0)) { onMicrorepeatOff(d1, ps); return; }
        }
        if (d1 == 65) {
            if (type == 0x90 && d2 > 0) { onSamplerBtn65Press(engine ? engine->sampler() : nullptr); return; }
            if (type == 0x80 || (type == 0x90 && d2 == 0)) { onSamplerBtn65Release(engine ? engine->sampler() : nullptr); return; }
        }
        if (d1 == 66) {
            if (type == 0x90 && d2 > 0) { onSamplerBtn66Press(); return; }
            if (type == 0x80 || (type == 0x90 && d2 == 0)) { onSamplerBtn66Release(engine ? engine->sampler() : nullptr); return; }
        }
        if (d1 < kApcRows * kApcCols) {
            if (type == 0x90 && d2 > 0) { onPadPress(d1, nowMs, ps, engine); return; }
            if (type == 0x80 || (type == 0x90 && d2 == 0)) { onPadRelease(d1, nowMs, ps, engine); return; }
        }
        // SHIFT-gated transport reroute: STOP_ALL (note 0x51/81) unshifted
        // uses the flat cmd/stopall map (handled by PluginProcessor's plain
        // CC/note passthrough, not here); shift+STOP_ALL = IMMEDIATE stop
        // that also aborts any in-progress recording.
        if (type == 0x90 && d2 > 0 && d1 == 0x51 && shiftHeld()) { onStopImmediate(ps); return; }
        // PLAY (note 0x5B/91) = CLEAR_ALL, intercepted here (not the flat
        // map) so this class's own shadow state resets in the same call
        // that wipes the DSP-side content -- CLEAR_ALL must always be
        // reachable regardless of shift state (loop-immediate is NOT
        // wired -- no addressable read head -- so there is nothing for
        // shift+PLAY to reroute to yet).
        if (d1 == 0x5B) {
            if (type == 0x90 && d2 > 0) { onClearAll(true, ps); return; }
            if (type == 0x80 || (type == 0x90 && d2 == 0)) { onClearAll(false, ps); return; }
        }
    }

    // channel 1 (keybed): any keybed key press engages live-pitch at that
    // key's own semitone offset (or triggers the sampler if it has content
    // for that key).
    if (channel == 1) {
        if (type == 0x90 && d2 > 0) { onKeybedNoteOn(d1, ps, engine ? engine->sampler() : nullptr); return; }
        if (type == 0x80 || (type == 0x90 && d2 == 0)) { onKeybedNoteOff(d1, engine ? engine->sampler() : nullptr); return; }
    }

    // --- everything else (filters, transport buttons, speed) via the flat
    // remap: CC48-55 -> fx/* knobs, plus cmd/* momentary note bindings. This
    // mirrors midi.cpp's config/controls.conf-driven flat map, hardcoded here
    // since a plugin has no on-disk config file to load at runtime (the same
    // bindings the shipped default controls.conf provides) -- see
    // PluginProcessor's AudioProcessorValueTreeState parameters for the
    // primary/automatable path; this flat map exists so a real APC Key25 (or
    // any MIDI controller sending these exact CC/note numbers) still works
    // identically when routed into the plugin's MIDI input.
    if (type == 0xB0) {
        float val = d2 / 127.0f;
        switch (d1) {
            case 48: ps.setByName("fx/reverb", val); return;
            case 49: ps.setByName("fx/delay", val); return;
            case 50: ps.setByName("fx/time", val); return;
            case 51: ps.setByName("fx/hp", val); return;
            case 54: ps.setByName("fx/lpres", val); return;
            case 55: ps.setByName("fx/lp", val); return;
            default: break;
        }
    }
    if ((type == 0x90 && d2 > 0) || type == 0x80 || (type == 0x90 && d2 == 0)) {
        float val = (type == 0x90 && d2 > 0) ? 1.0f : 0.0f;
        switch (d1) {
            case 0x0C: ps.setByName("cmd/halfspeed", val); return;
            case 0x0E: ps.setByName("cmd/doublespeed", val); return;
            case 0x03: case 0x02: ps.setByName("cmd/stopall", val); return;
            default: break;
        }
        // Per-track stop / erase (contiguous 20-slot families, matching
        // docs/COMMAND-SURFACE.md's TRACK_BASE/STOP_TRACK_BASE/ERASE_TRACK_BASE).
        if (d1 >= 0x40 && d1 < 0x40 + kLooperCount) {
            char z[32]; snprintf(z, sizeof z, "looper%d/play", d1 - 0x40);
            if (val > 0.5f) ps.setByName(z, 0.0f);
            return;
        }
        if (d1 >= 0x60 && d1 < 0x60 + kLooperCount) {
            char z[32]; snprintf(z, sizeof z, "looper%d/erase", d1 - 0x60);
            ps.setByName(z, val);
            return;
        }
    }
}

} // namespace aloopvst
