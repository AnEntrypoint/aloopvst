// LooperEngine — owns the generated Faust home stack (AloopLoopDsp, from
// FaustShim.h/loop.cpp), the sampler, and every per-block scratch buffer the
// orchestration needs. This is the JUCE-side replacement for aloop's
// src/dsp/audio_thread.cpp worker() function: same per-block algorithm (see
// LooperEngine.cpp's processBlock, ported line-for-line from worker()'s body),
// but driven from JUCE's processBlock callback instead of an ALSA/SCHED_FIFO
// thread, and reading host transport (AudioPlayHead) instead of Ableton Link.
//
// Real-time safety: every buffer here is sized and allocated exactly once, in
// prepare() (called from PluginProcessor::prepareToPlay). processBlock() does
// no malloc/lock/syscall, matching audio_thread.cpp's worker() discipline
// (docs/ARCHITECTURE.md's "Threading and real-time safety" section).
#pragma once

#include "FaustShim.h"
#include "ParamStore.h"
#include "sampler/sampler.h"

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace aloopvst {

// sampler/sampler.h declares its Sampler class inside `namespace aloop`
// (unchanged from aloop's own src/dsp/sampler/sampler.h -- see that header's
// own top-of-file comment: it's copied byte-identical, only the include
// guard is aloopvst-specific). Alias it into aloopvst so the rest of this
// port (LooperEngine, ApcControlSurface, PluginProcessor) can reference the
// unqualified `Sampler` name, matching aloop's own audio_thread.h/apc_grid.h
// (both written inside `namespace aloop`, where the name is naturally in
// scope) without needing `aloop::` qualification sprinkled through every
// call site here.
using aloop::Sampler;

constexpr int kLooperCount = 20;   // dsp/loop.dsp's NLOOPERS

// Per-looper state telemetry the editor UI and ApcControlSurface both read.
// Mirrors aloop's AudioThread::Telemetry (src/dsp/audio_thread.h) subset that
// is actually still meaningful inside a plugin (no ALSA xrun counter, no
// per-core busy%, no Link sync flag — those were hardware-shell concerns).
struct EngineTelemetry {
    bool  looperRec[kLooperCount]      = {};
    bool  looperPlay[kLooperCount]     = {};
    float looperVol[kLooperCount]      = {};
    float looperLevel[kLooperCount]    = {};   // 0..1, dsp/loop.dsp's "level" hbargraph
    float looperWriteIdx[kLooperCount] = {};   // dsp/loop.dsp's "writeidx" hbargraph -- ARM-quantization compensation read
    float inPeak  = 0.0f;
    float outPeak = 0.0f;
    float effSpeed = 1.0f;
    bool  monitorMode = false;   // SHIFT held (native fold engaged)
};

class LooperEngine {
public:
    LooperEngine() = default;

    // Heap-allocates the Faust DSP + sampler and every scratch buffer, then
    // calls AloopLoopDsp::init(sampleRate). NEVER stack-allocate faustHome_ --
    // see aloop's ADR-013 (a real 320MB stack-overflow SIGSEGV from exactly
    // this mistake on the hardware build; this plugin's MAXLEN is smaller but
    // the discipline is unconditional, not a size-dependent judgment call).
    // `params` is the ParamStore ApcControlSurface writes into -- owned by
    // PluginProcessor, passed in here so processBlock can read it every block
    // without any additional indirection.
    void prepare(double sampleRate, int maxBlockSize, ParamStore* params) {
        sampleRate_ = sampleRate;
        params_ = params;

        faustHomePtr_ = std::make_unique<AloopLoopDsp>();
        faustHome_ = faustHomePtr_.get();
        faustHome_->init((int)sampleRate);
        fui_ = std::make_unique<FaustUI>();
        faustHome_->buildUserInterface(fui_.get());

        samplerPtr_ = std::make_unique<Sampler>();

        const int N = maxBlockSize;
        fin_.assign((size_t)N, 0.0f);
        fout_.assign((size_t)N, 0.0f);
        prevLoopSum_.assign((size_t)N, 0.0f);
        prevFiltOut_.assign((size_t)N, 0.0f);
        clearBuf_.assign((size_t)N, 0.0f);
        speedBuf_.assign((size_t)N, 1.0f);
        masterPhaseBuf_.assign((size_t)N, 0.0f);
        masterLenBuf_.assign((size_t)N, 0.0f);
        rawGlitchTap_.assign((size_t)N, 0.0f);
        rawLoopSum_.assign((size_t)N, 0.0f);
        rawFiltTap_.assign((size_t)N, 0.0f);
        captureFin_.assign((size_t)N, 0.0f);
        samplerBuf_.assign((size_t)N, 0);

        foldGain_ = 0.0f;
        glitchFoldGain_ = 0.0f;
        masterPhaseSamples_ = 0.0;

        maxBlockSize_ = N;
    }

    // The per-block orchestration, ported from audio_thread.cpp's worker()
    // body (the section from "Apply the remappable controls" through the
    // prevLoopSum_/prevFiltOut_ snapshot at the end of the Faust compute()
    // call). `in`/`out` are mono float pointers of length `n` (n <= the
    // maxBlockSize passed to prepare()). `hostBpm`/`hostPpqPosition`/
    // `isPlaying` come from PluginProcessor's AudioPlayHead read -- the VST
    // replacement for audio_thread.cpp's LinkSnapshot reads. See
    // LooperEngine.cpp for the full, heavily-commented port.
    void processBlock(const float* in, float* out, int n,
                       double hostBpm, double hostPpqPosition, bool hostIsPlaying);

    // --- accessors for ApcControlSurface / PluginEditor ---------------------
    FaustUI& fui() { return *fui_; }
    const FaustUI& fui() const { return *fui_; }
    Sampler* sampler() { return samplerPtr_.get(); }
    EngineTelemetry snapshotTelemetry() const { return telemetry_; }
    double sampleRate() const { return sampleRate_; }

    // Read-only accessor for the "cmd/master_len" shared phrase length -- used
    // by ApcControlSurface's finish-quantization math the same way
    // apc_grid.cpp reads AudioThread's writeIdx telemetry (via
    // EngineTelemetry::looperWriteIdx above) and the ParamStore's
    // cmd/master_len entry.
    ParamStore* paramStore() { return params_; }

    // --- session persistence: ring-buffer content export/import -------------
    // See LooperEngine.cpp's exportLooperRing/importLooperRing for the full
    // derivation. Summary: dsp/loop.dsp's rwtable ring content has no direct
    // FaustUI reflection hook, but it IS reachable through the DSP's own
    // legitimate signal path --
    //   EXPORT: solo looper `looperIndex` (mute the other 19 via their `play`
    //   zone), hold `rec`=0/`in`=0/`prevFiltIn`=0/`effSpeed`=1.0 so nothing
    //   about its recorded state changes, and sweep the process()-level
    //   `masterPhase` INPUT linearly across a full `wrapLen`-sample span
    //   (`wrapLen` itself read back via the "finishtarget" zone, which
    //   loop.dsp's wrapLenStep latches to at every finishEdge -- see the
    //   comment at the read site). Since readPos == wrapAbs(masterPhase -
    //   recordStartPhaseOffset, wrapLen) at effSpeed==1.0 (dsp/loop.dsp lines
    //   421-447), sweeping masterPhase through any wrapLen consecutive
    //   integers visits every physical ring index exactly once (subtracting
    //   an unknown constant offset and reducing mod wrapLen is a bijection on
    //   Z/wrapLen) -- the CAPTURED ORDER is cyclically rotated by the
    //   (unknown, and irrelevant) recordStartPhaseOffset, but every sample is
    //   read via the ring's own real out=(record+hold)*playN*volN signal, so
    //   the captured audio IS the real recorded content, verbatim.
    //   IMPORT: re-inject the captured samples through the SAME write path a
    //   live recording uses -- a real ARM (rec 0->1) -> feed the captured
    //   buffer as `prevFiltIn` sample-by-sample across N-sample blocks with
    //   `wipe`=0 -> FINISH (finishreq pulse + finishtarget = captured length)
    //   cycle, exactly like ApcControlSurface's own applyRecPlayCycle. This
    //   is not an approximation of "how a recording gets made" -- it IS how a
    //   recording gets made, so recordStartPhaseOffset re-anchors correctly
    //   from the CURRENT masterPhase (via the real finishEdge this produces),
    //   and wrapLen re-latches to the real recorded length exactly as it
    //   would for a live take. The only thing NOT reproduced is the
    //   sample-for-sample PHYSICAL rotation the ring happened to be at when
    //   it was originally saved -- immaterial, since the loop plays back as
    //   the same repeating waveform cycle either way, freshly re-anchored to
    //   whatever the CURRENT session's phrase-lock is, exactly matching what
    //   a brand new recording of identical content would produce.
    // Neither direction ever triggers a spurious armEdge/finishEdge for any
    // OTHER looper, and never touches dsp/loop.dsp itself -- see
    // LooperEngine.cpp for the full safety argument (armPulse/armEdge only
    // fire from a genuine recN rising edge, never from masterPhase alone).
    //
    // Returns false (leaves outSamples empty) if looperIndex is out of range
    // or the looper has no recorded length (wrapLen would be 0/undefined).
    // Not real-time safe (runs many extra compute() blocks) -- callers must
    // only invoke this from getStateInformation/setStateInformation, never
    // from processBlock's hot path.
    bool exportLooperRing(int looperIndex, std::vector<int16_t>& outSamples);
    // Re-records `samples` into looper `looperIndex` via a real ARM->FINISH
    // cycle (see the derivation above). `samples` becomes the looper's new
    // recorded content, matching a live take of the exact same audio.
    void importLooperRing(int looperIndex, const std::vector<int16_t>& samples);

private:
    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 0;
    ParamStore* params_ = nullptr;   // not owned

    std::unique_ptr<AloopLoopDsp> faustHomePtr_;
    AloopLoopDsp* faustHome_ = nullptr;
    std::unique_ptr<FaustUI> fui_;
    std::unique_ptr<Sampler> samplerPtr_;

    // Per-block scratch (see prepare() for sizing; all allocated once).
    std::vector<float> fin_, fout_;
    std::vector<float> prevLoopSum_;      // previous block's RAW loop-engine output (SHIFT/glitch fold source)
    std::vector<float> prevFiltOut_;      // previous block's fully-effected mix (record-only tap source)
    std::vector<float> clearBuf_, speedBuf_, masterPhaseBuf_, masterLenBuf_;
    std::vector<float> rawGlitchTap_, rawLoopSum_, rawFiltTap_;
    std::vector<float> captureFin_;       // dry input + folded loop content, EXCLUDING this block's own sampler voices
    std::vector<int32_t> samplerBuf_;     // s32 scratch for captureBlock/renderInto

    float foldGain_ = 0.0f;
    float glitchFoldGain_ = 0.0f;
    double masterPhaseSamples_ = 0.0;

    EngineTelemetry telemetry_;

    static std::string targetToZone(const std::string& target);
};

} // namespace aloopvst
