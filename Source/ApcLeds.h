// ApcLeds — ports aloop's src/control/apc_leds.h/.cpp (APC Key25 LED output:
// per-pad color classification + a 30Hz coalescing cache) so a REAL APC Key25
// (or compatible) plugged in as the VST's MIDI input/output device gets the
// same LED feedback as the hardware unit. This is optional polish layered on
// top of the mandatory on-screen UI (PluginEditor) -- most users will drive
// aloopvst with no physical controller at all, and the editor's on-screen
// looper grid already shows every state this drives on real hardware LEDs.
// A VST3/CLAP host CAN route a plugin's MIDI output back to a physical
// device (JUCE's `producesMidi()` + a MidiBuffer written in processBlock),
// so this is a real, reachable feature, not a hardware-only capability.
#pragma once

#include "ApcControlSurface.h"
#include <cstdint>
#include <array>

namespace aloopvst {

// AKAI APC-series LED velocity constants (looper apcKey25.h:20-26 / midiMap.h:249-255).
enum ApcLedVel : uint8_t {
    kLedOff         = 0,
    kLedGreen       = 1,
    kLedGreenBlink  = 2,
    kLedRed         = 3,
    kLedRedBlink    = 4,
    kLedYellow      = 5,
    kLedYellowBlink = 6,
};

constexpr int kApcBtnStopAll = 0x51;    // 81 -- STOP_ALL indicator
constexpr int kApcBtnPlay    = 0x5B;    // 91 -- SHIFT-held indicator (yellow while held)
constexpr int kApcLiveLedNoteOut = 0x40;   // 64 -- live-pitch engage LED (velocity 127/0, not the ApcLedVel table)

// Owns the 128-slot coalescing cache. `write(note, velocity) -> bool` is a
// caller-supplied sink -- PluginProcessor appends a MIDI CC/note-on message to
// its output MidiBuffer, decoupling this module from JUCE's MIDI types
// directly (kept symmetric with aloop's own WriteFn design, which decoupled
// from ALSA rawmidi the same way).
class ApcLeds {
public:
    // `looperLevels` (may be null): 20-element array of per-looper live
    // output peak (0..1, dsp/loop.dsp's "level" hbargraph via
    // EngineTelemetry::looperLevel) -- drives the 3-tier VU-meter PLAY
    // coloring. Null degrades PLAY color to a flat GREEN.
    template <typename WriteFn>
    void refresh(unsigned nowMs, const ApcControlSurface& grid, bool liveEngaged,
                 WriteFn&& write, const float* looperLevels = nullptr) {
        // Boot delay: real APC hardware drops LED writes sent before it has
        // fully enumerated (apcKey25.h APC_LED_BOOT_DELAY_MS=2000, matching
        // aloop's own apc_leds.cpp). A VST's MIDI-out connection to a real
        // device goes through the same USB enumeration timing as the
        // hardware unit did, so the same guard applies here.
        if (!bootMs_) bootMs_ = nowMs ? nowMs : 1;
        if (nowMs - bootMs_ < kBootDelayMs) return;
        if (nowMs - lastMs_ < kRefreshMs) return;
        lastMs_ = nowMs;

        for (int row = 0; row < kApcRows; row++) {
            for (int col = 0; col < kApcCols; col++) {
                int note = row * kApcCols + col;
                sendCoalesced(note, gridColor(row, col, grid, looperLevels), write);
            }
        }
        sendCoalesced(kApcBtnPlay, grid.shiftHeld() ? kLedYellow : kLedOff, write);
        sendCoalesced(kApcLiveLedNoteOut, liveEngaged ? 127 : 0, write);
    }

    // Force a full re-send next refresh (matches aloop's invalidateLedCache,
    // used there on a USB-MIDI roster change) -- useful here if the host's
    // MIDI output routing changes mid-session (e.g. user connects the
    // physical APC after the plugin was already running).
    void invalidate() { cacheValid_.fill(false); bootMs_ = 0; }

private:
    static constexpr unsigned kBootDelayMs = 2000;
    static constexpr unsigned kRefreshMs = 33;   // ~30Hz

    unsigned bootMs_ = 0;
    unsigned lastMs_ = 0;
    std::array<uint8_t, 128> cache_{};
    std::array<bool, 128> cacheValid_{};

    template <typename WriteFn>
    void sendCoalesced(int note, uint8_t velocity, WriteFn&& write) {
        if (cacheValid_[note] && cache_[note] == velocity) return;
        if (write(note, velocity)) { cache_[note] = velocity; cacheValid_[note] = true; }
    }

    // Per-pad color classification, ported verbatim from aloop's
    // apc_leds.cpp gridColor() -- same VU-tier thresholds (raw s16-range
    // looper values 1500/8000, normalized to aloop's [-1,1] float range by
    // /32768), same MFS_* state->color mapping.
    uint8_t gridColor(int row, int col, const ApcControlSurface& grid, const float* looperLevels) const {
        static constexpr float kVuMid  = 1500.0f / 32768.0f;
        static constexpr float kVuHigh = 8000.0f / 32768.0f;
        int looper = gridLooperIndex(row, col);
        if (looper >= 0) {
            if (grid.looperRecording(looper))   return kLedRedBlink;
            if (!grid.looperHasContent(looper)) return kLedOff;
            if (grid.looperPlaying(looper)) {
                float level = looperLevels ? looperLevels[looper] : 0.0f;
                if (level >= kVuHigh) return kLedRed;
                if (level >= kVuMid)  return kLedYellow;
                return kLedGreen;
            }
            return kLedYellowBlink;
        }
        int preset = gridPresetIndex(row, col);
        if (preset >= 0) {
            return grid.presetUsed(preset) ? kLedYellow : kLedOff;
        }
        return kLedOff;
    }
};

} // namespace aloopvst
