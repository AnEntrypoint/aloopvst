// PluginEditor — the on-screen UI: a 5x4 grid of looper cells (rec/play/erase
// buttons + a level meter bar reading the Faust "level" hbargraph per
// looper), the 8 fx knobs as rotary sliders, microrepeat DIV as a segmented
// control, and SHIFT/glitch-fold state indicators. Backed by
// AudioProcessorValueTreeState for the fx knobs (host-automatable); the
// looper cells drive ApcControlSurface's ARM/FINISH state machine directly
// (a mouse click is treated as the same "pad press/release" gesture a MIDI
// note would produce -- see the button callbacks in PluginEditor.cpp) rather
// than writing straight into a parameter, matching docs/PARAMETER-MAP.md's
// explicit requirement that the state machine is never bypassed.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

namespace aloopvst {

class LooperCell : public juce::Component, private juce::Timer {
public:
    LooperCell(AloopAudioProcessor& proc, int looperIndex);
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;   // refresh the level meter + button highlight state
    void triggerPadGesture(bool press);

    AloopAudioProcessor& processor_;
    int looperIndex_;
    juce::TextButton recPlayButton_;   // single button, mirrors the hardware's single-pad tap cycle (ARM->FINISH->pause->resume)
    juce::TextButton eraseButton_;     // hold >=1000ms to erase, matching kHoldEraseMs -- mouse-held emulation, see .cpp
    float levelSmoothed_ = 0.0f;
    bool eraseHeldDown_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LooperCell)
};

class AloopAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit AloopAudioProcessorEditor(AloopAudioProcessor&);
    ~AloopAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;   // refresh SHIFT/glitch-fold indicators

    AloopAudioProcessor& processor_;

    std::array<std::unique_ptr<LooperCell>, kLooperCount> looperCells_;

    // 8 fx rotary knobs
    juce::Slider hpCutoffKnob_, lpCutoffKnob_, lpResonanceKnob_, reverbAmountKnob_,
                 delayAmountKnob_, timeKnob_, formantKnob_, semitonesKnob_;
    juce::Label hpCutoffLabel_, lpCutoffLabel_, lpResonanceLabel_, reverbAmountLabel_,
                delayAmountLabel_, timeLabel_, formantLabel_, semitonesLabel_;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> hpCutoffAttach_, lpCutoffAttach_,
        lpResonanceAttach_, reverbAmountAttach_, delayAmountAttach_, timeAttach_, formantAttach_, semitonesAttach_;

    // Microrepeat DIV as a segmented control (6 buttons: Off,1,2,4,8,16).
    // Wired manually (radio-group + explicit parameter writes in the .cpp),
    // not via ButtonAttachment -- JUCE's stock ButtonAttachment binds a
    // single bool parameter, but fx.microrepeatDiv is a 6-value
    // AudioParameterChoice, so each button's onClick writes the
    // corresponding choice index directly instead.
    juce::TextButton microDivButtons_[6];

    // Transport-ish toggles
    juce::TextButton halfSpeedButton_, doubleSpeedButton_, clearAllButton_, stopAllButton_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> halfSpeedAttach_, doubleSpeedAttach_, clearAllAttach_, stopAllAttach_;

    // SHIFT / glitch-fold state indicators (read-only, reflect
    // ApcControlSurface::shiftHeld()/LooperEngine's telemetry monitorMode)
    juce::Label shiftIndicator_, glitchIndicator_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AloopAudioProcessorEditor)
};

} // namespace aloopvst
