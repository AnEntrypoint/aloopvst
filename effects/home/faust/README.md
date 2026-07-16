# home-FX Faust chain (vendored from dubfx)

This is the verified dubfx Faust chain — the exact reproduction of the bare-metal
looper's effects, A/B-tested sample-for-sample (see `../../../docs/FEASIBILITY.md`
and the dubfx repo). It is vendored here unchanged (ADR-003) and packaged as the
home-FX LV2 bundle by CI (`.github/workflows/build-lv2.yml`).

- `chain.dsp` — the full chain: pitch → delay → reverb → microrepeat → HP/LP filters.
- `filters.dsp`, `delay.dsp`, `reverb.dsp`, `microrepeat.dsp`, `mixbus.dsp` — the stages.
- `pitch.dsp` + `pitch_ffi.h` — the live pitch stage. It links the EXACT C++ pitch
  engine via a Faust `ffunction` (ADR-004), so the sound is identical to looper.
- `soladSnacOctaver.h`, `grainFormant.h` — the vendored pitch engine headers from `../looper/patches/` (the ffunction target, ADR-004), kept flat next to the dsp so faust2lv2 resolves the `#include`s.
  `grainFormant.h`) from `../looper/patches/`. These are the `ffunction` target;
  the LV2 build compiles them in.
- `param_mapping.md` — the CC/note → normalized param mapping (from dubfx).

To rebuild locally (needs Faust + LV2 dev headers):
`faust2lv2 chain.dsp` → `chain.lv2/`.
