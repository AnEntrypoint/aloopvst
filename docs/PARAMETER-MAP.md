# Parameter map — VST parameter ID -> Faust zone -> MIDI source

This is the aloopvst-side companion to
[`../effects/home/faust/param_mapping.md`](../effects/home/faust/param_mapping.md)
(copied verbatim from aloop) and
[`COMMAND-SURFACE-REFERENCE.md`](COMMAND-SURFACE-REFERENCE.md). Those two
documents are the authoritative behavior spec (exact MIDI note/CC numbers,
normalization formulas, defaults). This table is just the ADDITIONAL mapping
a hardware-only design never needed: a host-automatable
`AudioProcessorValueTreeState` parameter ID for every control, so a DAW user
can automate/save these without a MIDI controller attached at all.

Every parameter's range/default is taken directly from the corresponding
Faust `hslider`/`nentry`/`button`/`checkbox` declaration (`dsp/effects_runtime.dsp`,
`dsp/loop.dsp`), so a parameter at its default reproduces the exact same
DSP state a freshly-booted aloop unit starts in (verified byte-exact
passthrough at these defaults per `param_mapping.md`).

| Parameter ID | Faust zone | Range | Default | Also settable via MIDI |
|---|---|---|---|---|
| `fx.hpCutoff` | `HPCUT` | 0.0–1.0 | 0.0 | CC51 |
| `fx.lpCutoff` | `LPCUT` | 0.0–1.0 | 1.0 | CC55 |
| `fx.lpResonance` | `LPRES` | 0.0–1.0 | 0.0 | CC54 |
| `fx.reverbAmount` | `REVAMT` | 0.0–1.0 | 0.0 | CC48 |
| `fx.delayAmount` | `DELAYAMT` | 0.0–1.0 | 0.0 | CC49 |
| `fx.time` | `TIME` | 0.0–1.0 | 0.5 | CC50 |
| `fx.formant` | `FORMANT` | -3.0–3.0 | 0.0 | CC53 (deadzone [60,68], SHIFT doubles range) |
| `fx.semitones` | `SEMIS` | -12.0–12.0 | 0.0 | keybed note (ch1), mod-wheel CC1, CC52 |
| `fx.microrepeatDiv` | `DIV` | {0,1,2,4,8,16} | 0 | notes 82–86 (latched, note-off clears) |
| `looperN.rec` | `looperN/rec` | bool | false | grid pad press (ARM/FINISH cycle) |
| `looperN.play` | `looperN/play` | bool | false | grid pad press cycle (pause/resume) |
| `looperN.erase` | `looperN/erase` | bool | false | grid pad hold >= 1000ms |
| `looperN.vol` | `looperN/vol` | 0.0–1.0 | 1.0 | (no MIDI source in the original; parameter-only) |
| `cmd.halfSpeed` | (native `speedBuf` signal input) | bool | false | dedicated hardware button; expose as a momentary VST parameter |
| `cmd.doubleSpeed` | (native `speedBuf` signal input) | bool | false | dedicated hardware button; expose as a momentary VST parameter |
| `cmd.clearAll` | (native `clearBuf` signal input) | bool | false | PLAY button unshifted (note 0x5B/91) |
| `cmd.stopAll` | (drives every `looperN/play`=0) | bool | false | STOP_ALL note 0x51/81 |

`N` ranges 0–19 (20 loopers, matching `dsp/loop.dsp`'s `NLOOPERS`). Loopers'
`rec`/`play`/`erase` are exposed as VST parameters for automation/session-save
visibility, but the actual ARM/FINISH/quantization STATE MACHINE (which of
those three fields to set on a given press, and when) still lives in
`ApcControlSurface`/`ApcGrid`-equivalent logic — a raw parameter write must
not bypass that state machine, or the ARM-quantization and finish-quantization
behaviors silently stop applying. Parameters are a mirror of the state
machine's output, not an alternate input path into the DSP zones directly
(same principle as the original hardware, where a MIDI note press goes
through `ApcGrid::onPadPress`, never straight into a Faust zone).
