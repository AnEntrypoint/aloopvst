// PluginEditor.cpp — see PluginEditor.h for the design summary.
//
// DESIGN DECISION: a LooperCell's mouse click is translated into the exact
// same synthetic MidiEvt a real APC Key25 pad press/release would produce
// (via gridLooperIndex's inverse -- looperIndex -> a note number that maps
// back to that same looper through ApcControlSurface::dispatchEvent), rather
// than calling ApcControlSurface's private applyRecPlayCycle directly. This
// keeps exactly ONE entry point into the ARM/FINISH state machine (dispatch
// via a decoded MidiEvt), so the on-screen UI and a real/virtual MIDI
// controller are provably indistinguishable to ApcControlSurface -- the same
// principle the parameter map's own note ("a raw parameter write must not
// bypass the state machine") applies to, just extended to mouse input too.
// gridLooperIndex(row,col) = row*4+(col-2) for col in [2,5] -- so any
// (row,col) with col=2 and row=looperIndex/4... actually the simplest exact
// inverse: pick row = looperIndex/4, col = 2 + (looperIndex%4), then the note
// number fed to dispatchEvent is row*kApcCols+col (matching dispatchEvent's
// own `row = note/kApcCols, col = note%kApcCols` decode).
#include "PluginEditor.h"
#include <cmath>

namespace aloopvst {

namespace {
    int looperIndexToNote(int looperIndex) {
        int row = looperIndex / 4;
        int col = 2 + (looperIndex % 4);
        return row * kApcCols + col;
    }
}

// ============================================================================
// LooperCell
// ============================================================================
LooperCell::LooperCell(AloopAudioProcessor& proc, int looperIndex)
    : processor_(proc), looperIndex_(looperIndex)
{
    recPlayButton_.setButtonText(juce::String(looperIndex + 1));
    recPlayButton_.setClickingTogglesState(false);
    recPlayButton_.onClick = [this] {
        // A mouse click is a single press+release pair, same as a real pad
        // tap (the ARM/FINISH/pause/resume cycle happens on PRESS for
        // arm/finish, and on RELEASE for pause/resume -- see
        // ApcControlSurface::onPadPress/onPadRelease). Fire both edges back
        // to back so a click behaves like an instantaneous tap.
        triggerPadGesture(true);
        triggerPadGesture(false);
    };
    addAndMakeVisible(recPlayButton_);

    eraseButton_.setButtonText("X");
    // Erase fires from a HELD mouse-down (see timerCallback(), which polls
    // eraseButton_.isDown() every tick), not a click -- mirrors the
    // hardware's physical hold gesture (kHoldEraseMs) rather than an
    // instantaneous tap. juce::Button::isDown() already tracks the mouse-down
    // state for us, so no separate mouseDown/mouseUp override is needed here.
    addAndMakeVisible(eraseButton_);

    startTimerHz(20);   // level meter refresh + hold-to-erase polling
}

void LooperCell::triggerPadGesture(bool press) {
    int note = looperIndexToNote(looperIndex_);
    MidiEvt ev{};
    ev.type = press ? 0x90 : 0x80;
    ev.channel = 0;
    ev.d1 = note;
    ev.d2 = press ? 127 : 0;
    auto nowMs = (unsigned)juce::Time::getMillisecondCounter();
    processor_.controlSurface().dispatchEvent(ev, nowMs, processor_.paramStore(), &processor_.engine());
}

void LooperCell::resized() {
    auto b = getLocalBounds().reduced(2);
    auto eraseArea = b.removeFromRight(24);
    eraseButton_.setBounds(eraseArea);
    recPlayButton_.setBounds(b);
}

void LooperCell::paint(juce::Graphics& g) {
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.2f));
    g.fillRect(b);

    // Level meter bar along the bottom of the cell, reading
    // EngineTelemetry::looperLevel (dsp/loop.dsp's "level" hbargraph).
    auto meterArea = b.removeFromBottom(6.0f).reduced(2.0f, 0.0f);
    g.setColour(juce::Colours::darkgrey);
    g.fillRect(meterArea);
    g.setColour(juce::Colours::limegreen);
    g.fillRect(meterArea.withWidth(meterArea.getWidth() * juce::jlimit(0.0f, 1.0f, levelSmoothed_)));

    // Button colour reflects state (empty / recording / playing / paused),
    // matching apc_leds.cpp's own 3-tier classification in spirit (no LED
    // hardware here, but the same has-content/recording/playing states).
    bool recording = processor_.controlSurface().looperRecording(looperIndex_);
    bool playing = processor_.controlSurface().looperPlaying(looperIndex_);
    bool hasContent = processor_.controlSurface().looperHasContent(looperIndex_);
    juce::Colour c = juce::Colours::darkgrey;
    if (recording) c = juce::Colours::red;
    else if (playing) c = juce::Colours::limegreen;
    else if (hasContent) c = juce::Colours::orange;
    recPlayButton_.setColour(juce::TextButton::buttonColourId, c);
}

void LooperCell::timerCallback() {
    EngineTelemetry t = processor_.engine().snapshotTelemetry();
    float target = t.looperLevel[looperIndex_];
    // Simple UI-side smoothing so the meter doesn't flicker at 20Hz redraw
    // (the Faust-side ba.slidingMax envelope already smooths the underlying
    // value; this is just an additional visual ease).
    levelSmoothed_ += (target - levelSmoothed_) * 0.3f;

    // Hold-to-erase: while the mouse is held down on the erase button, poll
    // elapsed time against kHoldEraseMs -- mirrors the hardware's physical
    // hold gesture. ApcControlSurface's own pollHolds() only fires an erase
    // from ITS OWN m_looperHeld/m_looperHoldStart state, which is set by a
    // genuine pad press dispatch -- so a mouse-held erase gesture here is
    // translated into a synthetic pad press (held) that ApcControlSurface's
    // existing pollHolds() timing threshold then fires exactly like a real
    // hardware long-press, instead of duplicating the 1000ms threshold logic
    // a second time in the UI.
    bool nowHeld = eraseButton_.isDown();
    if (nowHeld && !eraseHeldDown_) {
        // Mouse-down edge: fire the synthetic pad PRESS once, exactly like a
        // hardware pad going down -- ApcControlSurface's own
        // pollHolds()/kHoldEraseMs timing (driven every processBlock) then
        // fires the actual erase once >=1000ms have elapsed while held,
        // identically to a real hardware long-press. No separate 1000ms
        // timer is duplicated here.
        triggerPadGesture(true);
    } else if (!nowHeld && eraseHeldDown_) {
        // Mouse-up edge: release, matching onPadRelease's own tap-vs-hold
        // disambiguation (a short hold that never reached kHoldEraseMs just
        // cycles rec/play like a normal tap -- ApcControlSurface itself
        // already handles that, nothing extra needed here).
        triggerPadGesture(false);
    }
    eraseHeldDown_ = nowHeld;

    repaint();
}

// ============================================================================
// AloopAudioProcessorEditor
// ============================================================================
namespace {
    void setupKnob(juce::Slider& knob, juce::Label& label, const juce::String& text, juce::Component& parent) {
        knob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
        parent.addAndMakeVisible(knob);
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        parent.addAndMakeVisible(label);
    }
}

AloopAudioProcessorEditor::AloopAudioProcessorEditor(AloopAudioProcessor& p)
    : AudioProcessorEditor(&p), processor_(p)
{
    for (int i = 0; i < kLooperCount; i++) {
        looperCells_[(size_t)i] = std::make_unique<LooperCell>(processor_, i);
        addAndMakeVisible(*looperCells_[(size_t)i]);
    }

    auto& apvts = processor_.apvts();
    setupKnob(hpCutoffKnob_, hpCutoffLabel_, "HPCUT", *this);
    setupKnob(lpCutoffKnob_, lpCutoffLabel_, "LPCUT", *this);
    setupKnob(lpResonanceKnob_, lpResonanceLabel_, "LPRES", *this);
    setupKnob(reverbAmountKnob_, reverbAmountLabel_, "REVAMT", *this);
    setupKnob(delayAmountKnob_, delayAmountLabel_, "DELAYAMT", *this);
    setupKnob(timeKnob_, timeLabel_, "TIME", *this);
    setupKnob(formantKnob_, formantLabel_, "FORMANT", *this);
    setupKnob(semitonesKnob_, semitonesLabel_, "SEMIS", *this);

    hpCutoffAttach_    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "fx.hpCutoff", hpCutoffKnob_);
    lpCutoffAttach_    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "fx.lpCutoff", lpCutoffKnob_);
    lpResonanceAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "fx.lpResonance", lpResonanceKnob_);
    reverbAmountAttach_= std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "fx.reverbAmount", reverbAmountKnob_);
    delayAmountAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "fx.delayAmount", delayAmountKnob_);
    timeAttach_        = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "fx.time", timeKnob_);
    formantAttach_     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "fx.formant", formantKnob_);
    semitonesAttach_   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "fx.semitones", semitonesKnob_);

    // Microrepeat DIV segmented control: 6 mutually-exclusive buttons backed
    // by the "fx.microrepeatDiv" AudioParameterChoice (docs/PARAMETER-MAP.md).
    // ButtonAttachment expects a toggle-style button per choice index isn't
    // directly supported by JUCE's stock ButtonAttachment (which binds a
    // single bool parameter) -- since microrepeatDiv is a multi-value choice,
    // wire the 6 buttons manually via a RadioGroup + explicit parameter
    // writes instead of ButtonAttachment (left as nullptr / unused).
    static const char* divLabels[6] = { "Off", "1", "2", "4", "8", "16" };
    auto* microParam = apvts.getParameter("fx.microrepeatDiv");
    for (int i = 0; i < 6; i++) {
        microDivButtons_[i].setButtonText(divLabels[i]);
        microDivButtons_[i].setRadioGroupId(1001);
        microDivButtons_[i].setClickingTogglesState(true);
        microDivButtons_[i].onClick = [this, i, microParam] {
            if (microParam) {
                float norm = microParam->convertTo0to1((float)i);
                microParam->setValueNotifyingHost(norm);
            }
        };
        addAndMakeVisible(microDivButtons_[i]);
    }
    microDivButtons_[0].setToggleState(true, juce::dontSendNotification);

    halfSpeedButton_.setButtonText("1/2x");
    doubleSpeedButton_.setButtonText("2x");
    clearAllButton_.setButtonText("CLEAR ALL");
    stopAllButton_.setButtonText("STOP ALL");
    halfSpeedButton_.setClickingTogglesState(true);
    doubleSpeedButton_.setClickingTogglesState(true);
    clearAllButton_.setClickingTogglesState(true);
    stopAllButton_.setClickingTogglesState(true);
    addAndMakeVisible(halfSpeedButton_);
    addAndMakeVisible(doubleSpeedButton_);
    addAndMakeVisible(clearAllButton_);
    addAndMakeVisible(stopAllButton_);
    halfSpeedAttach_   = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(apvts, "cmd.halfSpeed", halfSpeedButton_);
    doubleSpeedAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(apvts, "cmd.doubleSpeed", doubleSpeedButton_);
    clearAllAttach_    = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(apvts, "cmd.clearAll", clearAllButton_);
    stopAllAttach_     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(apvts, "cmd.stopAll", stopAllButton_);

    shiftIndicator_.setText("SHIFT", juce::dontSendNotification);
    shiftIndicator_.setJustificationType(juce::Justification::centred);
    shiftIndicator_.setColour(juce::Label::backgroundColourId, juce::Colours::darkgrey);
    addAndMakeVisible(shiftIndicator_);

    glitchIndicator_.setText("GLITCH", juce::dontSendNotification);
    glitchIndicator_.setJustificationType(juce::Justification::centred);
    glitchIndicator_.setColour(juce::Label::backgroundColourId, juce::Colours::darkgrey);
    addAndMakeVisible(glitchIndicator_);

    setSize(900, 620);
    startTimerHz(20);
}

AloopAudioProcessorEditor::~AloopAudioProcessorEditor() = default;

void AloopAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AloopAudioProcessorEditor::resized() {
    auto area = getLocalBounds().reduced(10);

    // Looper grid: 5 rows x 4 cols, top ~60% of the window.
    auto gridArea = area.removeFromTop((int)(area.getHeight() * 0.55f));
    const int cols = 4;
    const int rows = (kLooperCount + cols - 1) / cols;   // 5 rows for 20 loopers at 4 cols
    int cellW = gridArea.getWidth() / cols;
    int cellH = gridArea.getHeight() / rows;
    for (int i = 0; i < kLooperCount; i++) {
        int row = i / cols, col = i % cols;
        looperCells_[(size_t)i]->setBounds(gridArea.getX() + col * cellW, gridArea.getY() + row * cellH, cellW - 4, cellH - 4);
    }

    area.removeFromTop(10);

    // fx knobs row
    auto knobRow = area.removeFromTop(110);
    juce::Slider* knobs[8] = { &hpCutoffKnob_, &lpCutoffKnob_, &lpResonanceKnob_, &reverbAmountKnob_,
                               &delayAmountKnob_, &timeKnob_, &formantKnob_, &semitonesKnob_ };
    juce::Label* labels[8] = { &hpCutoffLabel_, &lpCutoffLabel_, &lpResonanceLabel_, &reverbAmountLabel_,
                               &delayAmountLabel_, &timeLabel_, &formantLabel_, &semitonesLabel_ };
    int knobW = knobRow.getWidth() / 8;
    for (int i = 0; i < 8; i++) {
        auto cell = knobRow.removeFromLeft(knobW);
        labels[i]->setBounds(cell.removeFromTop(16));
        knobs[i]->setBounds(cell);
    }

    area.removeFromTop(10);

    // Microrepeat DIV segmented control
    auto divRow = area.removeFromTop(30);
    int divW = divRow.getWidth() / 6;
    for (int i = 0; i < 6; i++) microDivButtons_[i].setBounds(divRow.removeFromLeft(divW).reduced(2));

    area.removeFromTop(10);

    // Transport row + indicators
    auto transportRow = area.removeFromTop(30);
    int tw = transportRow.getWidth() / 6;
    halfSpeedButton_.setBounds(transportRow.removeFromLeft(tw).reduced(2));
    doubleSpeedButton_.setBounds(transportRow.removeFromLeft(tw).reduced(2));
    clearAllButton_.setBounds(transportRow.removeFromLeft(tw).reduced(2));
    stopAllButton_.setBounds(transportRow.removeFromLeft(tw).reduced(2));
    shiftIndicator_.setBounds(transportRow.removeFromLeft(tw).reduced(2));
    glitchIndicator_.setBounds(transportRow.removeFromLeft(tw).reduced(2));
}

void AloopAudioProcessorEditor::timerCallback() {
    bool shift = processor_.controlSurface().shiftHeld();
    shiftIndicator_.setColour(juce::Label::backgroundColourId, shift ? juce::Colours::yellow : juce::Colours::darkgrey);

    EngineTelemetry t = processor_.engine().snapshotTelemetry();
    bool glitchActive = processor_.controlSurface().microrepeatDiv() > 0;
    glitchIndicator_.setColour(juce::Label::backgroundColourId, glitchActive ? juce::Colours::orange : juce::Colours::darkgrey);
    juce::ignoreUnused(t);

    // Reflect the CURRENT fx.microrepeatDiv parameter value on the segmented
    // control -- keeps the UI correct if the value changes via MIDI (notes
    // 82-86, routed through ApcControlSurface, not this parameter) or host
    // automation rather than only via this control's own button clicks.
    if (auto* microParam = processor_.apvts().getParameter("fx.microrepeatDiv")) {
        int idx = (int)std::lround(microParam->convertFrom0to1(microParam->getValue()));
        for (int i = 0; i < 6; i++)
            microDivButtons_[i].setToggleState(i == idx, juce::dontSendNotification);
    }
}

} // namespace aloopvst
