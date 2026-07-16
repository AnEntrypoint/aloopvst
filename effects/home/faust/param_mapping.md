# Parameter mapping — CC/note → normalized → chain constant

The looper's effect params arrive as MIDI CC/note values and are normalized before
reaching the DSP. The Faust chain takes the **already-normalized** values as its
per-render constants (SEMIS/FORMANT/ENGAGED/DELAYAMT/REVAMT/TIME/DIV/MLB/HPCUT/
LPCUT/LPRES). The normalization below is the control-plane mapping the looper
applies (apcKey25*.cpp); the audio path (this chain) is verified against the
normalized values, so reproducing these mappings makes any knob position match.

| Knob | MIDI source | Normalization (C++) | Chain param |
|------|-------------|---------------------|-------------|
| HP cutoff | CC51 | `data2/127` | HPCUT ∈ [0,1] |
| LP resonance | CC54 | `data2/127` | LPRES ∈ [0,1] |
| LP cutoff | CC55 | `data2/127` | LPCUT ∈ [0,1] |
| Reverb amount | CC48 | `data2/127` | REVAMT ∈ [0,1] |
| Delay amount | CC49 | `data2/127` | DELAYAMT ∈ [0,1] |
| Time | CC50 | `data2/127` | TIME ∈ [0,1] |
| Formant depth | CC53 | deadzone data2∈[60,68]→0; else `((data2-64)/63)*range`, range=1 (or 3 w/ SHIFT) | FORMANT ∈ [-3,3] |
| Pitch semitones | keybed note (ch1) | `note-60` | SEMIS |
| " | mod wheel CC1 | deadzone 59-69→disengage; else `(data2-64)*12/63` | SEMIS |
| " | CC52 | `(data2/127)*24-12` | SEMIS |
| Pitch scale | (derived) | `pow(2, semitones/12)` | (computed in pitch.dsp) |
| Engaged | note/CC engage | boolean | ENGAGED ∈ {0,1} |
| Microrepeat div | notes 82-86 | latched → {1,2,4,8,16}; note-off clears | DIV |
| masterLoopBlocks | Link/loop grid | phrase length in blocks | MLB |
| THRU/LOOP/MIX vol | CC (base 0x08) | `value/63` | (mix-bus stage) |
| INPUT/OUTPUT gain | CC | `value/127` → codec | (codec, out of DSP path) |

Defaults (the passthrough state): HPCUT=0, LPCUT=1, LPRES=0, REVAMT=0, DELAYAMT=0,
TIME=0.5, FORMANT=0, SEMIS=0, ENGAGED=0, DIV=0, MLB=0, THRU/LOOP/MIX=1.0. At these
the whole chain is a byte-exact passthrough (verified maxAbs=0).
