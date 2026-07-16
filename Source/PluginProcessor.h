// PluginProcessor — the JUCE AudioProcessor that hosts LooperEngine +
// ApcControlSurface inside a DAW. This is the JUCE-side replacement for
// aloop's main.cpp (which spawned AudioThread::start()/runMidiLoop() on
// separate ALSA/SCHED_FIFO threads): here, JUCE's own processBlock callback
// IS the per-block RT thread the host already provides, so there is no
// separate thread to spawn -- see docs/ARCHITECTURE.md.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "LooperEngine.h"
#include "ApcControlSurface.h"
#include "ParamStore.h"
#include "ApcLeds.h"

namespace aloopvst {

class AloopAudioProcessor : public juce::AudioProcessor {
public:
    AloopAudioProcessor();
    ~AloopAudioProcessor() override;

    // --- AudioProcessor lifecycle -------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // --- editor --------------------------------------------------------------
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // --- plugin metadata -----------------------------------------------------
    const juce::String getName() const override { return "aloop"; }
    bool acceptsMidi() const override { return true; }
    // Produces MIDI output for APC Key25 LED feedback (ApcLeds, optional --
    // only meaningful if the host actually routes this plugin's MIDI output
    // back to a real connected device; harmless if not).
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    // --- session persistence --------------------------------------------------
    // Serializes all 20 loopers' recorded ring-buffer contents + rec/play/
    // erase state + fx params + masterLen/tempo, so a DAW project save/reload
    // doesn't silently lose every recorded loop -- new work the hardware
    // version never needed (it is always-on, so nothing is ever "reloaded").
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // --- accessors the editor reads each timer tick ---------------------------
    LooperEngine& engine() { return engine_; }
    ApcControlSurface& controlSurface() { return controlSurface_; }
    ParamStore& paramStore() { return paramStore_; }
    juce::AudioProcessorValueTreeState& apvts() { return apvts_; }

    // Called from the message thread (LooperCell button clicks in
    // PluginEditor.cpp) to inject a synthetic pad gesture. MUST NOT call
    // controlSurface_.dispatchEvent() directly from here -- ApcControlSurface
    // has no internal thread-safety of its own (its plain, non-atomic member
    // state was designed for every dispatch to happen from processBlock's
    // audio-thread MIDI iteration, matching aloop's own runMidiLoop-drives-
    // ApcGrid single-thread-owns-this-state design). WITNESSED (this
    // session, via a diagnostic harness that isolated the engine from the
    // GUI): the engine's actual record/finish/play logic is fully correct
    // when driven from a single thread -- the reported "loopers lit up but
    // weren't audible" symptom traced to exactly this data race: a mouse
    // click's dispatchEvent() call landing on the message thread while
    // processBlock's own dispatchEvent/pollHolds calls run concurrently on
    // the audio thread, corrupting ApcControlSurface's un-synchronized
    // state. Queuing here and draining only inside processBlock (see its
    // own comment) gives the UI click path the exact same single-writer
    // guarantee real MIDI input already has.
    void pushUiMidiEvent(const MidiEvt& ev) {
        const juce::ScopedLock sl(uiMidiLock_);
        if (pendingUiMidi_.size() < 256) pendingUiMidi_.push_back(ev);
    }

private:
    // Drained once per processBlock call, on the audio thread, BEFORE the
    // host's own MidiBuffer iteration -- see processBlock's own comment for
    // why draining here (not from a click callback) is what actually fixes
    // the race.
    juce::CriticalSection uiMidiLock_;
    std::vector<MidiEvt> pendingUiMidi_;

public:

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    // Pushes the current APVTS parameter values into paramStore_ (the same
    // ParamStore ApcControlSurface/LooperEngine read every block) -- called
    // once per processBlock so host automation reaches the DSP the identical
    // way a MIDI CC would (see docs/PARAMETER-MAP.md's note: parameters
    // mirror the ApcControlSurface state machine's OUTPUT for the fx/* knobs,
    // which have no state-machine gating, but must never bypass the ARM/
    // FINISH state machine for looperN.rec/play/erase -- so those three are
    // NOT pushed this way, see the method body for the exact split).
    void pushAutomatableParamsToStore();

    LooperEngine engine_;
    ApcControlSurface controlSurface_;
    ParamStore paramStore_;
    juce::AudioProcessorValueTreeState apvts_;

    // Optional APC Key25 LED feedback -- refreshed once per block from
    // processBlock, writing coalesced note-on messages into the host MIDI
    // output buffer (see PluginProcessor.cpp's processBlock). Harmless if no
    // physical device is routed to this plugin's MIDI output: the messages
    // just go nowhere.
    ApcLeds apcLeds_;

    // Edge-detect state for the APVTS "cmd.clearAll" momentary parameter (see
    // pushAutomatableParamsToStore()/processBlock's clear-all bridge) -- a
    // plain member instead of a function-local static so multiple plugin
    // instances never share state.
    bool prevClearHeld_ = false;

    // Running sample counter (since prepareToPlay), used to derive a
    // monotonic "now_ms" for ApcControlSurface's tap-vs-hold timing --
    // sample-accurate and independent of wall-clock scheduling jitter,
    // replacing aloop's CLOCK_MONOTONIC read on a separate control thread.
    juce::int64 samplesElapsed_ = 0;

    // Scratch mono buffers for the stereo-in-summed-to-mono / duplicated-back
    // conversion (docs/ARCHITECTURE.md's "Decision: stereo-in-summed-to-mono"
    // section) -- sized once in prepareToPlay, never reallocated in
    // processBlock (RT-safety).
    std::vector<float> monoIn_;
    std::vector<float> monoOut_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AloopAudioProcessor)
};

} // namespace aloopvst
