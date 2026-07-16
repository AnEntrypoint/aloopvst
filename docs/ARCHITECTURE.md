# aloopvst architecture

This project ports [aloop](../aloop) — a bare-metal-derived Raspberry Pi hardware
looper pedal — into a VST3/Standalone plugin that runs inside any DAW. The goal
is behavioral parity: same loop engine, same effects chain, same command
surface, same recently-debugged quantization/varispeed/fold behaviors. What
changes is everything *around* the DSP that only makes sense on dedicated
hardware.

See [`CLONE-PARITY-REFERENCE.md`](CLONE-PARITY-REFERENCE.md) and
[`COMMAND-SURFACE-REFERENCE.md`](COMMAND-SURFACE-REFERENCE.md) (copied verbatim
from aloop) for the authoritative behavior spec this plugin implements. When in
doubt about what a control or quantization rule is supposed to do, those two
documents plus aloop's own `dsp/loop.dsp`/`dsp/aloop.dsp` comments are the
source of truth — not this file, which only explains what's *different* for
the plugin form factor.

## What carries over unchanged

- **The Faust DSP** (`dsp/loop.dsp`, `dsp/aloop.dsp`, `dsp/effects_runtime.dsp`,
  `effects/home/faust/*.dsp`) — vendored byte-identical from aloop. This IS the
  loop engine (20 independent record/play loopers, no overdub, ARM/finish
  quantization, phrase-locked playback) and the dubfx effect chain
  (pitch/delay/reverb/microrepeat/filters). Compiled to C++ the same way aloop
  does: `faust -lang cpp -cn AloopLoopDsp dsp/aloop.dsp`.
- **The sampler** (`Source/sampler/sampler.h`) — aloop's chromatic + drum
  sampler was already portable hardware-independent C++; copied unchanged.
- **The command surface** — the same note/CC numbers, the same tap-vs-hold
  timing, the same ARM-quantization/finish-quantization math, the same
  SHIFT-fold/GLITCH-fold mix. See `Source/ApcControlSurface.*`.

## What's different, and why

| aloop (hardware) | aloopvst (plugin) | Why |
|---|---|---|
| ALSA capture/playback on a dedicated SCHED_FIFO RT thread | JUCE host callback (`processBlock`) | A plugin has no business spawning its own RT audio thread — the host already provides one, with its own scheduling guarantees. |
| Ableton Link (UDP multicast peer discovery) drives `masterPhase`/tempo | The host's own transport (`AudioPlayHead` — bpm, ppq, isPlaying) | A plugin runs inside a DAW that already has a tempo/transport; there's no peer network to discover from inside a single plugin instance. |
| Physical APC Key25 MIDI controller is the only control surface | The DAW's routed MIDI input (same note/CC numbers) PLUS an on-screen editor UI PLUS host-automatable parameters | A plugin can't assume a specific hardware controller is attached. The on-screen UI is the primary surface; the APC Key25 mapping still works identically if a user routes one to the plugin's MIDI input; APC LED feedback is an optional bonus if a real device is connected. |
| Dynamically-loaded user LV2 on a free CPU core | (dropped) | The DAW itself is the modular host — a user chains their own plugins in the DAW's own graph. Re-hosting LV2 inside a plugin sandbox would duplicate what the host already does better. |
| Always-on appliance, no save/load concept | `getStateInformation`/`setStateInformation` | A DAW project save/reload must not silently lose every recorded loop — this is new work the hardware version never needed. |
| Fixed 48kHz, fixed block size | Sample-rate- and block-size-agnostic (`prepareToPlay` reinitializes the Faust DSP with the host's real values) | A plugin host can run at any sample rate/block size, and can change both at runtime. |

## Threading and real-time safety

`processBlock` follows the exact same no-malloc/no-lock/no-syscall discipline
as aloop's `audio_thread.cpp` worker loop. The generated `AloopLoopDsp` and the
sampler's buffers are heap-allocated exactly once, in `prepareToPlay`
(`std::make_unique`), never as a stack-local and never re-allocated per block —
aloop's own ADR-013 documents a real SIGSEGV from getting this wrong on a
smaller stack budget; the same discipline applies here even though the
plugin's smaller `MAXLEN` makes the object far smaller than aloop's ~320MB
instance.

## Decision: stereo-in-summed-to-mono, mono-duplicated-to-stereo-out

aloop's DSP core is mono internally: `audio_thread.cpp` deinterleaves the
USB wire's stereo channels to mono by averaging (`acc / wireCh`) before
handing samples to Faust, and duplicates the mono Faust output back onto
both wire channels afterward. aloopvst matches this exactly rather than
inventing a "proper stereo" loop engine: the plugin declares a stereo
input bus and a stereo output bus (matching what most DAW tracks expect by
default), but `processBlock` sums L+R to mono the same way
(`(L+R)/2`) before feeding the Faust `in` signal, and copies the single
mono Faust output to both L and R after `compute()` — byte-for-byte the
same math as the hardware unit, just replacing "USB wire channels" with
"host bus channels". A genuinely stereo loop engine (independent L/R
content per looper) would be a different instrument, not a port of this one.

## Decision: no standalone Ableton Link mode

aloopvst does not integrate the Ableton Link library, even as an opt-in extra.
The host DAW's own transport already gives every looper a shared, sample-accurate
tempo/phase reference (see the table above) — the one thing Link would add on
top is syncing to *other, separate* Link-enabled apps/devices outside the DAW
entirely, which is a materially different use case from what aloop's hardware
needed Link for (aloop had no DAW/host transport at all, so Link was its only
possible shared clock). If that cross-app sync need becomes concrete later,
it's a new, separately-scoped feature request, not a gap in this port.
