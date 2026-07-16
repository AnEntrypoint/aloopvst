// ffunction bridge exposing the EXACT ../looper EngineSoladSnac pitch shifter
// to the Faust chain. Faust calls these per sample; a single static engine
// instance (matching the looper's mono usage — one EngineSoladSnac, fed the
// mono signal) holds all state across calls.
//
// The looper drives the engine as: setPitchScale(pow(2,semis/12)); then
// processBlock(in,out,n) per audio block, only when engaged (else bypass). Here
// we expose:
//   dubfx_pitch_set(scale, formantDepth, engaged)  -- set params (idempotent)
//   dubfx_pitch_tick(x)                             -- process one sample, return out
// When engaged==0 the tick is a pass-through (matching loopMachine's bypass:
// when !liveEngaged the wrapper is skipped entirely). On the 0->1 engage edge
// we call reengage() exactly as RubberBandWrapper::setEngaged does.
//
// This is the EXACT C++ engine (soladSnacOctaver.h), so the pitch stage is
// bit-identical to the looper by construction — not an approximation.

#ifndef DUBFX_PITCH_FFI_H
#define DUBFX_PITCH_FFI_H

#include "soladSnacOctaver.h"

// Single mono engine instance (the looper runs one engine on the mono signal;
// RubberBandWrapper's two L/R engines are identical on identical input, so one
// suffices and matches the mono output the looper keeps).
static EngineSoladSnac& dubfx_engine() {
    static EngineSoladSnac eng;
    return eng;
}

static float& dubfx_lastScale()   { static float s = 1.0f; return s; }
static bool&  dubfx_lastEngaged() { static bool e = false; return e; }

// Apply params to the engine (idempotent). Mirrors RubberBandWrapper:
// setPitchScale + setFormant + setEngaged (reengage on false->true edge).
static inline void dubfx_pitch_apply(float scale, float formantDepth, float engaged) {
    EngineSoladSnac& e = dubfx_engine();
    if (scale != dubfx_lastScale()) { e.setPitchScale(scale); dubfx_lastScale() = scale; }
    e.setFormantDepth(formantDepth);
    bool eng = engaged >= 0.5f;
    if (eng && !dubfx_lastEngaged()) e.reengage();
    dubfx_lastEngaged() = eng;
}

// CRITICAL: the engine's SNAC cadence uses the block size n directly
// (m_sinceDetect += n at soladSnacOctaver.h:430), so processBlock(...,1) per
// sample would drift from the looper's processBlock(...,64). We therefore
// buffer exactly BS=64 input samples and call processBlock(buf,out,64) once,
// EXACTLY as loopMachine does, then serve the 64 outputs one at a time. Faust
// runs -bs 64 in lockstep, so tick() is called 64 times per block in order.
//
// This introduces a 1-block (64-sample) latency: input sample of block k is
// buffered, processed when the block fills, and its output served across the
// NEXT 64 tick calls. The C++ reference harness must apply the same 1-block
// framing for an apples-to-apples A/B (see refharness pitch stage).
static const int DUBFX_BS = 64;

// Per-sample tick with a 1-block (64-sample) framing latency: input sample i of
// a block is buffered; when the block fills (i==63) it is processed as one
// processBlock(...,64) call (exact SNAC cadence); the resulting 64 outputs are
// served across the NEXT 64 ticks. So tick returns outBuf[pos] for the block
// processed one block ago. The reference harness applies the identical 1-block
// delay so the A/B is aligned.
// Params are passed IN with the sample (Faust would elide a separate param-only
// call whose result is unused — a *0 fold). We apply them here each sample, then
// process. scale=pow(2,semis/12); formant depth; engaged (>=0.5).
extern "C" inline float dubfx_pitch_tick(float x, float scale, float formant, float engaged) {
    dubfx_pitch_apply(scale, formant, engaged);

    static float inBuf[DUBFX_BS];
    static float outBuf[DUBFX_BS];
    static int   pos = 0;    // position within the current block (0..63)

    if (!dubfx_lastEngaged()) return x;  // bypass (looper skips engine)

    float y = outBuf[pos];   // output from the block processed one block ago
    inBuf[pos] = x;          // buffer this input
    pos++;
    if (pos >= DUBFX_BS) {
        dubfx_engine().processBlock(inBuf, outBuf, DUBFX_BS);  // exact block call
        pos = 0;
    }
    return y;
}

#endif
