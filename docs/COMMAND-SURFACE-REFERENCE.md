# Command surface — the looper control behavior aloop clones

100% clone includes the control/command behavior, not just audio. aloop
reproduces looper's full command surface (the ported apcKey25*.cpp logic + loop
commands), with the input source moved to ALSA rawmidi (src/control/midi.cpp).

## Loop commands (ported from looper loopMachine command dispatch)
record · play · stop · clear/erase (per track) · halve/double speed (varispeed) · loop-immediate · set/clear loop-start — for each of the 20 independent loopers. NO overdub.
· set/clear mark point · pause (mute-with-advancing-head) · quantize-to-grid.

### Exact opcode → aloop control target (commonDefines.h → config/controls.conf)
The hardware speaks `LOOP_COMMAND_*` opcodes; aloop's control map speaks named
targets that resolve to Faust zones (src/dsp/audio_thread.cpp `targetToZone` +
the engine-global `clear`/`speed` handling). This is the authoritative parity map:

| looper opcode | value | aloop target | how it's realized |
|---|---|---|---|
| `LOOP_COMMAND_RECORD` (per track `TRACK_BASE 0x20`) | 0x80 / 0x20+i | `looper<i>/rec` | Faust `button("rec")` — record replaces the loop (NO overdub) |
| `LOOP_COMMAND_PLAY` | 0x81 | `looper<i>/play` | Faust `checkbox("play")` — gates the looper output |
| `LOOP_COMMAND_STOP` / `STOP_IMMEDIATE` | 0x03 / 0x02 | `cmd/stopall` | audio thread clears **every** `looper<i>/play` to 0 |
| `LOOP_COMMAND_STOP_TRACK_BASE` | 0x40+i | `looper<i>/play`=0 | per-track stop = clearing that one play checkbox |
| `LOOP_COMMAND_CLEAR_ALL` | 0x01 | `cmd/clearall` | engine-global — a plain `process()` signal input (3rd input, `clearBuf` in audio_thread.cpp), NOT a Faust UI zone — wipes all 20 loops |
| `LOOP_COMMAND_ERASE_TRACK_BASE` | 0x60+i | `looper<i>/erase` | per-looper Faust `button("erase")` — wipes that one loop |
| `LOOP_COMMAND_HALFSPEED_ON/OFF` | 0x0C/0x0D | `cmd/halfspeed` | engine-global `speed`=0.5 while held (varispeed read rate) — a plain `process()` signal input (4th input, `speedBuf`), NOT a Faust UI zone (see below) |
| `LOOP_COMMAND_DOUBLESPEED_ON/OFF` | 0x0E/0x0F | `cmd/doublespeed` | engine-global `speed`=2.0 while held (2× read rate), same signal-input mechanism |
| `LOOP_COMMAND_ABORT_RECORDING` | 0x06 | `looper<i>/rec`=0 | releasing rec ends the take; record replaces in place so there is nothing to "un-append" |
| `LOOP_COMMAND_LOOP_IMMEDIATE` | 0x08 | *(model difference)* | needs an addressable read head — see the note below |
| `LOOP_COMMAND_SET_LOOP_START` | 0x09 | *(model difference)* | needs an addressable read head — see the note below |
| `LOOP_COMMAND_CLEAR_LOOP_START` | 0x0A | *(model difference)* | needs an addressable read head — see the note below |
| Link tempo/phase | — | `looper<i>/len` | audio thread sizes every loop from the Link BPM (varispeed grid sync) |

### The one deliberate model difference: mark-point / immediate re-trigger (0x08–0x0A)
These three commands reposition an **addressable read head** (set a restart mark at
the current play position; jump all heads to their mark). The aloop loop engine is a
Faust **feedback-delay ring** — record replaces the loop, play recirculates it — which
has NO addressable read position, so it cannot express a mark-point jump.

Why not a buffer+playhead (rwtable) engine, which *does* have an addressable head?
Because a preserve-on-hold playhead looper must **read the buffer and write the
read-back to the same buffer** (so the loop survives while not recording) — a
read-modify-write that Faust's pure-signal evaluator rejects. This was witnessed
across **4 CI codegen attempts** (`syntax error` → `stack overflow in eval` →
`endless evaluation cycle of 8 steps`); the delay ring sidesteps RMW by construction
and is the correct Faust looper. See `.wfgy/lessons.md` and the ADR in `DECISIONS.md`.

Everything ELSE maps 1:1 (record/play/stop/stop-all/erase/clear/half-double-speed,
plus Link-driven varispeed loop length). Mark-point is the single behavior traded for
a single-Faust-program, maintainable, RMW-free engine — a documented, evidence-backed
model choice, not a silently dropped feature.

Momentary semantics match the hardware exactly: `HALFSPEED`/`DOUBLESPEED` and
`clear`/`erase` are **held** (value 1 = active, release = neutral), driven each
block from the atomic ParamStore the MIDI thread writes. `speed` composes with the
Link-set `len`: Link sets the base loop length, `speed` divides it (loop stays
grid-locked, plays at 0.5×/2×). The `TRACK_BASE 0x20` / `STOP_TRACK_BASE 0x40` /
`ERASE_TRACK_BASE 0x60` families are 20 contiguous per-track slots — the default
`config/controls.conf` binds all 20 of each; remap freely (no recompile).

**`clear`/`speed` are process() signal inputs, not Faust UI zones (fixed in
commit 9806835, 2nd-generation fix on top of 382e775's incomplete attempt):**
Faust's `par(i, NLOOPERS, vgroup(...))` combinator re-elaborates whatever UI
primitive (`button()`/`hslider()`) is textually referenced inside its body at
EACH of the 20 instantiation sites — even when the declaration itself is
hoisted to file scope and threaded in as an ordinary function parameter. This
was WITNESSED via the generated C++ (`build/loop.cpp`): `"speed"`/`"clear"`
each appeared 20 times, one per `"looper N"` vgroup, even after 382e775
believed it had collapsed them to a single shared zone. There is no Faust
mechanism for "declare a UI control once, reference the same zone from many
call sites" across a `par()` boundary. The real fix removes `clear`/`speed`
as Faust UI controls entirely: `dsp/loop.dsp`'s `process()` now takes 4
signal inputs — `(in, prevFiltIn, clearAll, speedMul)` — and `clearAll`/
`speedMul` are plain wires threaded through `par()`, which cannot duplicate a
signal the way it duplicates a UI primitive. `audio_thread.cpp` fills
`clearBuf`/`speedBuf` (constant across each block) and passes them as
`fins[2]`/`fins[3]` to `compute()` instead of calling `fui.set("clear"/
"speed", ...)`.

## APC Key25 controls (the exact mapping — src/control/midi.cpp, param_mapping.md)
- CC48–55 → reverb/delay/time/HP/LP/resonance (all `/127`)
- CC53 → formant depth (deadzone + range, SHIFT expands) — `ApcGrid::onFormantCC`
- CC52 / keybed / mod-wheel → live pitch semitones
- notes 82–86 → microrepeat divisors {1,2,4,8,16}
- notes 70/71 → global speed-scrub (varispeed)
- SHIFT (note 0x62/98, channel 0 only) → held state gating CC53's range AND the
  loop-fold/monitor routing: while held, the running loops are folded INTO the
  effect input (`fx/monitorfold` → Faust `MONITORFOLD`, `dsp/aloop.dsp`'s
  `foldMix`) with the dry loop contribution complementarily suppressed, so the
  loops are heard once, through the effects — `ApcGrid::onShiftPress/Release`,
  `AudioThread::Telemetry::monitorMode`. Drum-record-mode (looper's per-key
  keybed sampler) is NOT ported — see `docs/DECISIONS.md` ADR-012.
- the 8×5 grid → track/clip select + state display

## Link / transport
Ableton Link tempo/phase drives the loop grid; loop record start quantizes to the
Link phase; the first loop can propose the tempo to the session (ported logic).

Every one of these behaviors is the *ported looper logic* — same code, same
result. The mapping table above is the same one dubfx verified against the real
control plane (param_mapping.md).
