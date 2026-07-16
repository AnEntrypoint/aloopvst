# aloopvst

A VST3/Standalone plugin port of [aloop](../aloop), a hardware looper pedal.
See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for what carries over
unchanged (the Faust DSP, the sampler, the command surface) versus what's
different for the plugin form factor (host transport instead of Ableton Link,
on-screen UI + host-automatable parameters instead of a fixed hardware
controller, DAW session save/load, no dynamically-loaded user LV2 slot).

`docs/CLONE-PARITY-REFERENCE.md` and `docs/COMMAND-SURFACE-REFERENCE.md` are
copied verbatim from aloop's own `docs/` — they are the authoritative behavior
spec this plugin implements. Before changing any quantization, varispeed, or
fold-mix behavior, read the corresponding comments in `dsp/loop.dsp` and
`dsp/aloop.dsp` first — that history documents several real regressions from
getting these subtly wrong.
