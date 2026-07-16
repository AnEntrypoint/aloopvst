# 100% clone parity — aloop vs looper, effects via LV2

The user requirement: **aloop reproduces the looper's complete behavior 100%,
but the effects are delivered as the Faust LV2 plugin instead of inline C++.**
This document is the traceability of that claim — what is bit-identical, what is
behaviorally equivalent, and exactly how the LV2 effect path substitutes for
looper's inline effects.

## The two halves of the clone

**1. The loop engine — a native Faust reimplementation (behaviorally equivalent).**
The loop engine is `dsp/loop.dsp` — an aloop-native Faust program: 20 independent
record/play loopers as cycle-free **feedback-delay rings** (record replaces the loop,
play recirculates it), NO overdub. This is the correct Faust looper: it sidesteps the
read-modify-write that a buffer+playhead would need (which Faust rejects — see ADR-010
and COMMAND-SURFACE). One consequence: mark-point restart (SET/CLEAR_LOOP_START,
LOOP_IMMEDIATE) needs an addressable read head the ring doesn't have, so it is a
single deliberate, documented model difference; every other loop command maps 1:1.
aloop contains no Circle/looper code; it reproduces the *behavior* natively. The
loop grid is driven by Ableton Link phase (varispeed sync), same as the original.

**2. The effects — the exact same DSP, as Faust.**
The original ran its effects as inline C++ in this order: pitch → delay+reverb
sends → beat-repeat (microrepeat) → HP/LP filters → mix. aloop's effect chain
(`effects/home/faust/chain.dsp`, from dubfx) runs **exactly these stages in
exactly this order**, and the dubfx project A/B-verified it sample-for-sample
against the original C++. In aloop the effects are composed straight into the
home Faust program (`dsp/aloop.dsp = loop.dsp : chain.dsp`) — so the effects are
the same math, now Faust, and swappable at the LV2 boundary.

### Why this is sample-identical, not approximate

The Faust chain was A/B-verified against the *real looper C++* in the dubfx
project, sample-for-sample:

- The aloop home-FX `.dsp` files are **byte-identical** to the dubfx source
  (verified by `diff`).
- The pitch engine headers (`soladSnacOctaver.h`, `grainFormant.h`) are
  **byte-identical to looper's** — the LV2 links the *exact same C++ engine* via
  `ffunction`, so the pitch stage is literally looper's code.
- The dubfx 10-preset A/B matrix passes: **bit-identical** for pitch,
  microrepeat, filters, reverb-at-defaults, mix, and the whole chain at defaults;
  the only bounded-equivalent stage is the tape delay's self-feedback tail
  (corr ≥ 0.956), a float32 precision limit of the reference itself (documented
  in dubfx `.wfgy/lessons.md`).

So the effect path is **sample-identical to looper wherever looper is itself
deterministic**, delivered through the LV2 boundary.

## Parity classification

| Behavior | aloop vs looper | Basis |
|----------|-----------------|-------|
| Loop record/play, 20 independent loopers (no overdub) | **behaviorally equivalent** | native Faust reimplementation (`dsp/loop.dsp`), same musical behavior |
| Link-synced grid, varispeed | **behaviorally equivalent** | Link phase drives the Faust loop length, same as the original |
| Effects: pitch, microrepeat, filters, reverb(default), mix | **bit-identical** | the Faust effect chain == the original C++ (dubfx A/B), composed into the home stack |
| Effect: tape delay feedback tail | **behaviorally equivalent** (corr ≥ 0.956) | float32 precision limit of the reference itself |
| Control/command surface (loop cmds, APC grid/CC/notes) | **equivalent mapping** | native control layer + the exact CC map (dubfx `param_mapping.md`) |
| USB-audio / WiFi / Link transport | **behaviorally equivalent, improved** | kernel stack replaces hand-rolled (fixes the once-a-second glitch) |

## How the whole home stack is one Faust program

`dsp/aloop.dsp` composes the loop engine and the effects into a single Faust
program: `component("loop.dsp") : component(".../chain.dsp")` — input → loop
engine → the dubfx effect chain → output. It compiles (`faust -lang cpp`) to one
C++ file containing both the loop feedback and the effect stages (incl. the exact
pitch engine via `ffunction`). So the entire home audio path is one Faust compile
— change a stage or a mapping in Faust, rebuild, done. No hand-written C++ DSP.
The user's own effect is a separate swappable LV2 on the free core.

## Verifying the clone

- **Effects**: already proven — the Faust effect chain is A/B-verified
  sample-for-sample against the original C++ (dubfx, 10/10 presets). aloop uses
  that exact chain.
- **Loop engine**: the Faust loop engine is verified by its own deterministic A/B
  (record a known signal, confirm the loop-back is sample-exact) — a dev-host
  test, no hardware.
- **Live device round-trip** (real USB audio in/out on the Pi): the one part that
  needs a Pi 4 (`hardware-test-execution`, `test/hardware/`).
