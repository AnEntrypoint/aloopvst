// ApcControlSurface — ports aloop's src/control/apc_grid.h/.cpp (the ApcGrid
// state machine: ARM/FINISH per-looper record-play cycling, SHIFT held-state,
// microrepeat latch, live-pitch mod-wheel/keybed, sampler record-arm buttons,
// clear-all, stop-immediate) PLUS src/control/midi.cpp's note/CC dispatch
// table (the part of midi.cpp that decides WHICH ApcGrid method a given
// MIDI byte triggers) onto a JUCE MidiBuffer consumed once per processBlock,
// instead of a blocking ALSA rawmidi read loop on a separate control thread.
//
// Every note number, CC number, tap-vs-hold timing threshold (kHoldEraseMs),
// grid indexing (gridLooperIndex/gridPresetIndex), and the ARM-quantization /
// finish-quantization target computation (deriveTempoQuant's candidate
// search, extend-vs-backdate logic) are preserved VERBATIM from apc_grid.cpp
// -- these are recently-debugged (see aloop's own git log: "fix
// finish-quantization backdate case", "ARM-quantization -- snap recording
// start to the phrase grid", "fix multiple-length quantization", "route
// SHIFT/glitch-folded loop content into sampler recording") and must not be
// silently reimplemented differently.
//
// What's genuinely different from apc_grid.cpp/midi.cpp:
//   - `now_ms` comes from JUCE's own sample-accurate transport (running
//     sample count / sample rate), not a wall-clock CLOCK_MONOTONIC read on a
//     separate polling thread -- pollHolds() is driven once per processBlock
//     call (block-rate polling) instead of on every ALSA MIDI byte / 100ms
//     poll timeout, since a plugin has no separate control thread to poll on.
//     Block-rate polling is at least as fine-grained as the original's 100ms
//     poll for any reasonable block size (e.g. 512 samples @48kHz = ~10.6ms).
//   - No ALSA rawmidi/LED output -- APC Key25 LED feedback is dropped for v1
//     (see the design-decision note in ApcControlSurface.cpp's bindAll()).
//   - LinkBridge* is replaced by nullptr-safe host-tempo accessors: the
//     "two-way Link tempo proposal" (proposeTempo) has no VST equivalent (a
//     plugin cannot set its host's tempo), so it becomes a documented,
//     harmless no-op retained only so ported call sites keep the exact same
//     shape as apc_grid.cpp (see ApcControlSurface.cpp's onProposeTempo()).
//   - AudioThread* (for writeIdx telemetry reads) is replaced by a direct
//     LooperEngine* pointer -- same mechanism (a read-only snapshot struct),
//     different owning class.
#pragma once

#include "ParamStore.h"
#include "LooperEngine.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

namespace aloopvst {

constexpr int kApcRows = 5;
constexpr int kApcCols = 8;
constexpr int kPresetCount = 10;
constexpr unsigned kHoldEraseMs = 1000;     // apc_grid.h APC_HOLD_ERASE_MS
constexpr int kApcSampleRate = 48000;       // nominal SR used for the ms<->sample conversions ported from apc_grid.cpp;
                                             // ApcControlSurface itself is sample-rate-agnostic at the JUCE level (see prepare())
constexpr long kMaxLoopSamplesDefault = 48000 * 60;  // dsp/loop.dsp's MAXLEN -- the delay ring's hard ceiling (at 48kHz; scaled in prepare())
constexpr int kApcBtnShift = 0x62;          // apc_grid.h APC_BTN_SHIFT (98) -- channel 0 only
constexpr int kApcLiveLedNote = 0x40;       // note 64: live-pitch master engage toggle ("transpose on/off")
constexpr int kSamplerBaseNote = 48;        // Sampler::BASE_NOTE, channel-1 keybed low key

// row*8+col grid index -> looper index (cols 2-5) or -1 (apcKey25Notes.cpp's _looperFromPad, ported via apc_grid.h)
inline int gridLooperIndex(int row, int col) {
    if (row < 0 || row >= kApcRows) return -1;
    if (col < 2 || col > 5) return -1;
    int idx = row * 4 + (col - 2);
    return (idx >= 0 && idx < kLooperCount) ? idx : -1;
}
// row*8+col grid index -> preset index (cols 0-1) or -1 (apcKey25Notes.cpp's _presetFromPad)
inline int gridPresetIndex(int row, int col) {
    if (row < 0 || row >= kApcRows) return -1;
    if (col < 0 || col > 1) return -1;
    int idx = row * 2 + col;
    return (idx >= 0 && idx < kPresetCount) ? idx : -1;
}

// A single decoded MIDI event, already split into status/channel/d1/d2 --
// the plugin-side equivalent of midi.cpp's byte-at-a-time state machine
// (st/d1/d2/phase), fed from JUCE's already-assembled MidiBuffer messages
// instead of raw rawmidi bytes.
struct MidiEvt {
    int type;      // 0x80 note-off, 0x90 note-on, 0xB0 CC
    int channel;   // 0-15
    int d1;        // note number or CC number
    int d2;        // velocity or CC value
};

class ApcControlSurface {
public:
    // Register every target name this engine can ever write, ONCE, before
    // processBlock starts reading (called from PluginProcessor::prepareToPlay,
    // same moment LooperEngine::prepare() runs). ParamStore::bind() takes
    // bindMtx but setByName/get/forEach do NOT -- calling bind() from a hot
    // dispatch path would race processBlock's unlocked forEach over the same
    // unordered_map, exactly as apc_grid.h's own doc comment warns. Pre-binding
    // here and never bind()ing again from onPadPress/onShiftPress/etc keeps
    // the "bind at startup, read-only after" invariant.
    void bindAll(ParamStore& ps);

    // Feed one block's worth of already-decoded MIDI events (PluginProcessor
    // iterates the host MidiBuffer and calls this once per message), then
    // polls holds once (block-rate, replacing the original's 100ms poll
    // timeout / per-MIDI-byte poll). `nowMs` is a monotonic ms counter
    // PluginProcessor derives from the running sample count (see
    // PluginProcessor.cpp) so timing stays sample-accurate and independent of
    // wall-clock scheduling jitter. `engine` (may be null defensively, should
    // always be valid once prepareToPlay has run) supplies the writeIdx
    // telemetry read-back for finish-quantization and the Sampler pointer for
    // note 65/66/keybed routing.
    void dispatchEvent(const MidiEvt& ev, unsigned nowMs, ParamStore& ps, LooperEngine* engine);
    void pollHolds(unsigned nowMs, ParamStore& ps);

    // Public entry point for PluginProcessor's APVTS "cmd.clearAll" parameter
    // edge (docs/PARAMETER-MAP.md) -- routes through the SAME onClearAll the
    // MIDI PLAY-button dispatch uses (see dispatchEvent's note-0x5B handling),
    // so a host-automated clear-all resets ApcControlSurface's own shadow
    // state exactly like a MIDI-triggered one, never bypassing it via a raw
    // ParamStore write (see apc_grid.cpp's own history for the class of bug
    // that bypassing this exact reset causes).
    void requestClearAll(bool held, ParamStore& ps) { onClearAll(held, ps); }

    // Configure the sample rate used for BPM<->sample-count conversions in
    // the finish-quantization bracket search. Does NOT rescale
    // kMaxLoopSamples_ with sample rate -- see that field's own comment for
    // why: dsp/loop.dsp's `rwtable(MAXLEN, ...)` bakes MAXLEN=48000*60 as a
    // Faust COMPILE-TIME constant into the generated ring buffer size, so the
    // ring's real capacity is always exactly 2,880,000 SAMPLES regardless of
    // the host's actual sample rate (30s of real time at 96kHz, ~65.3s at
    // 44.1kHz, not a fixed 60s at every rate). Previously this scaled the
    // ceiling AS IF it were 60 real-time seconds at any host rate
    // (`60.0 * sr`), which at e.g. 96kHz computed a ceiling of 5,760,000 --
    // roughly double the ring's true 2,880,000-sample capacity -- letting
    // the quantization bracket search propose lengths the DSP would have
    // silently truncated (dsp/loop.dsp's writeIdx clamps to
    // `min(prev+1, MAXLEN-1)` with no error signal back to this layer).
    // Fixed: kMaxLoopSamples_ is the fixed sample-count constant, always.
    void setSampleRate(double sr) { sampleRate_ = sr; }

    // Restore per-looper shadow state after PluginProcessor::setStateInformation
    // pushes the raw Faust zone values back in directly (bypassing the
    // ARM/FINISH dispatch path, which is correct for a session RELOAD -- there
    // is no press to replay). Without this, a freshly-constructed
    // ApcControlSurface's m_looperHasContent/m_looperPlaying/m_looperRecording
    // shadow would stay all-false even for a looper the reloaded state marks
    // as having content, so the NEXT pad press would incorrectly ARM a fresh
    // recording (treating it as empty) instead of pause/resume-cycling the
    // restored content -- the same class of shadow/DSP desync apc_grid.cpp's
    // own history warns about for a clear-all that bypasses onClearAll.
    void restoreLooperShadowState(int looper, bool hasContent, bool playing, bool recording) {
        if (looper < 0 || looper >= kLooperCount) return;
        m_looperHasContent[looper] = hasContent;
        m_looperPlaying[looper] = playing;
        m_looperRecording[looper] = recording;
    }
    // Restore the shared master-phrase-length shadow (mirrors cmd/master_len)
    // -- same reasoning as restoreLooperShadowState: without this, the next
    // FIRST-establish-branch check in applyRecPlayCycle would incorrectly
    // treat a reloaded session's already-established phrase as "not yet
    // established" until the next block's ParamStore resync happens to run
    // first (a race, not a guarantee).
    void restoreMasterLenSamples(long samples) { m_masterLenSamples = samples; }

    bool liveEngaged() const { return m_liveEngaged; }
    bool shiftHeld() const { return m_shift; }
    bool drumRecordMode() const { return m_drumRecordMode; }
    bool looperHasContent(int looper) const { return m_looperHasContent[looper]; }
    bool looperPlaying(int looper) const { return m_looperPlaying[looper]; }
    bool looperRecording(int looper) const { return m_looperRecording[looper]; }
    bool presetUsed(int preset) const { return m_presetUsed[preset]; }
    uint8_t microrepeatDiv() const { return m_microRepeatDiv; }

private:
    // --- state, mirroring apc_grid.h's private members verbatim -------------
    bool m_looperHeld[kLooperCount] = {};
    unsigned m_looperHoldStart[kLooperCount] = {};
    bool m_looperErased[kLooperCount] = {};
    unsigned m_looperEraseReleaseAt[kLooperCount] = {};
    unsigned m_looperFinishReqReleaseAt[kLooperCount] = {};
    bool m_looperArmedOnPress[kLooperCount] = {};
    bool m_looperPlaying[kLooperCount] = {};
    bool m_looperHasContent[kLooperCount] = {};
    bool m_looperRecording[kLooperCount] = {};
    unsigned m_recordStartMs[kLooperCount] = {};

    bool m_presetHeld[kPresetCount] = {};
    unsigned m_presetHoldStart[kPresetCount] = {};
    bool m_presetCaptured[kPresetCount] = {};
    bool m_presetUsed[kPresetCount] = {};
    uint32_t m_presetMask[kPresetCount] = {};

    uint8_t m_microRepeatDiv = 0;
    bool m_shift = false;
    bool m_liveEngaged = false;
    bool m_drumRecordMode = false;
    long m_masterLenSamples = 0;

    double sampleRate_ = 48000.0;
    long kMaxLoopSamples_ = kMaxLoopSamplesDefault;

    // --- per-pad / per-command handlers, ported 1:1 from apc_grid.cpp -------
    void applyRecPlayCycle(int looper, unsigned now_ms, ParamStore& ps, LooperEngine* engine);
    void capturePreset(int p, ParamStore& ps);
    void applyPreset(int p, ParamStore& ps);
    void forgetLooperFromPresets(int looper);

    void onPadPress(int note, unsigned now_ms, ParamStore& ps, LooperEngine* engine);
    void onPadRelease(int note, unsigned now_ms, ParamStore& ps, LooperEngine* engine);
    void onModWheel(uint8_t data2, ParamStore& ps);
    void onAbsolutePitch(uint8_t data2, ParamStore& ps);
    void onLiveEngageToggle(ParamStore& ps);
    void onKeybedNoteOn(int note, ParamStore& ps, Sampler* sampler);
    void onKeybedNoteOff(int note, Sampler* sampler);
    void onSamplerBtn65Press(Sampler* sampler);
    void onSamplerBtn65Release(Sampler* sampler);
    void onSamplerBtn66Press();
    void onSamplerBtn66Release(Sampler* sampler);
    void onStopImmediate(ParamStore& ps);
    void onClearAll(bool held, ParamStore& ps);
    void onMicrorepeatOn(int note, ParamStore& ps);
    void onMicrorepeatOff(int note, ParamStore& ps);
    void onShiftPress(ParamStore& ps);
    void onShiftRelease(ParamStore& ps);
    void onFormantCC(uint8_t data2, ParamStore& ps);
};

} // namespace aloopvst
