// Microrepeat (beat-repeat / stutter) — exact port of microRepeat::process.
//
// For a LATCHED div (constant per render) with a given masterLoopBlocks:
//   sliceLen = (masterLoopBlocks/16/div) * 64   (floor >=1 block), capped 48000
//   On engage: capture live audio into a ring for the first sliceLen samples,
//   then replay the ring (loop) from readPos=0, wrapping at sliceLen.
//   out = live*(1-wet) + rep*wet
//   wet ramps by STEP=1/16 per BLOCK (64 samples) toward 1 (engaged), per-sample
//   interpolated. During capture, rep == live (seamless first pass).
//
// This render models a STEADY latched div (wet=1 after ramp) — the common
// audible case. The block-rate wet ramp is reproduced via a per-sample counter.
// Capture+replay uses a rwtable: write live during capture, read at readPos.

import("stdfaust.lib");

SR   = 48000.0;
BS   = 64;          // AUDIO_BLOCK_SAMPLES
STEP = 1.0/16.0;    // wet ramp per block

DIV = 0;            // 0=off, else 1/2/4/8/16
MLB = 0;            // masterLoopBlocks (0 or <16 => disabled)

MR_MAX = 48000;

// sliceLen in samples: beatBlocks=MLB/16; sliceBlocks=beatBlocks/div (>=1);
// sliceLen=sliceBlocks*64 capped 48000. Computed as a constant per render.
// DIV=0 (off) must not divide by zero — guard the divisor to 1 (sliceLen is
// irrelevant when active is false).
divSafe     = max(1, DIV);
beatBlocks  = int(MLB / 16);
sliceBlocks = max(1, int(beatBlocks / divSafe));
sliceLenRaw = sliceBlocks * BS;
sliceLen    = min(MR_MAX, sliceLenRaw);

active = (DIV != 0) & (MLB >= 16);

// WITNESSED live: sampleIdx counting from PROGRAM START (not from when
// active last became true) meant that by the time a user engages glitch
// minutes into a real session, sampleIdx is already far past any realistic
// sliceLen -- `capturing` (the brief capture-fresh-content window) could
// essentially NEVER trigger, so the ring was read from (rwtable's
// zero-initialized/stale memory) without ever having been written this
// engage. As `wet` ramped toward 1 (mrProcess below), the output converged
// on that silent/stale `rep`, i.e. glitch engaging made ALL audio
// disappear -- exactly the reported "glitch currently takes away loop
// audio, all audio disappears". Fixed: track samples-since-LAST-ENGAGE
// (reset to 0 on active's rising edge) instead of samples-since-program-
// start, so `capturing` reliably covers the first sliceLen samples of
// EVERY fresh engage, not just one that happens to occur in the program's
// first sliceLen samples ever.
activePrev = active : mem;
engageEdge = active & (activePrev < 0.5);   // active this sample, NOT active last sample
sampleIdx = counter ~ _
with {
    counter(prev) = ba.if(engageEdge, 0, prev + 1);
};

// Capture phase: the first `sliceLen` samples record live into the ring.
capturing = active & (sampleIdx < sliceLen);

// Ring: write live during capture at capturePos (=sampleIdx while capturing),
// read at replay readPos = sampleIdx % sliceLen (wraps). During capture the
// read == live (first pass). We use a rwtable of size MR_MAX.
// rwtable(size, initSig, writeIdx, writeSig, readIdx)
ringRead(live) = rep
with {
    // Write index: during capture it is sampleIdx (0..sliceLen-1). AFTER capture
    // it MUST NOT keep overwriting captured slots — park it at MR_MAX-1 (a slot
    // never read, since rpos < sliceLen <= MR_MAX-1) so replay reads intact data.
    wpos = int(ba.if(capturing, sampleIdx, MR_MAX - 1));
    rpos = int(ba.if(sliceLen > 0, sampleIdx % max(1, sliceLen), 0));
    stored = rwtable(MR_MAX, 0.0, wpos, live, rpos);
    // During capture, replayed signal IS live (seamless); after, it's the ring.
    rep = ba.if(capturing, live, stored);
};

// Wet ramp — EXACT per-sample interpolation matching the C++:
//   per block, wetStart = m_wet (block-start value), wetEnd = wetStart±STEP
//   (clamped to target), and within the block wet += (wetEnd-wetStart)/64 each
//   sample. So at block b, sample s: wet = wetStart_b + (wetEnd_b-wetStart_b)*s/64.
// When active (target=1): wetStart_b = min(1, b*STEP), wetEnd_b = min(1,(b+1)*STEP).
blockIdx    = int(sampleIdx / BS);
sampInBlock = sampleIdx - blockIdx*BS;             // 0..63
wetStartB   = min(1.0, blockIdx * STEP);
wetEndB     = min(1.0, (blockIdx + 1) * STEP);
wet = ba.if(active, wetStartB + (wetEndB - wetStartB) * (sampInBlock / BS), 0.0);

mrProcess(live) = live*(1.0 - wet) + rep*wet
with {
    rep = ringRead(live);
};

process = mrProcess;
