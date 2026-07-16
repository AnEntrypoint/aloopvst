// PluginProcessor.cpp — JUCE AudioProcessor wiring for LooperEngine +
// ApcControlSurface. See PluginProcessor.h and docs/ARCHITECTURE.md for the
// design; this file is intentionally thin -- almost all real DSP/control
// behavior lives in LooperEngine.cpp/ApcControlSurface.cpp, ported from
// aloop's audio_thread.cpp/apc_grid.cpp respectively.
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdint>

namespace aloopvst {

// --- parameter IDs, matching docs/PARAMETER-MAP.md verbatim -----------------
namespace ParamID {
    static const juce::String hpCutoff       = "fx.hpCutoff";
    static const juce::String lpCutoff       = "fx.lpCutoff";
    static const juce::String lpResonance    = "fx.lpResonance";
    static const juce::String reverbAmount   = "fx.reverbAmount";
    static const juce::String delayAmount    = "fx.delayAmount";
    static const juce::String time           = "fx.time";
    static const juce::String formant        = "fx.formant";
    static const juce::String semitones      = "fx.semitones";
    static const juce::String microrepeatDiv = "fx.microrepeatDiv";
    static const juce::String halfSpeed      = "cmd.halfSpeed";
    static const juce::String doubleSpeed    = "cmd.doubleSpeed";
    static const juce::String clearAll       = "cmd.clearAll";
    static const juce::String stopAll        = "cmd.stopAll";
}

AloopAudioProcessor::AloopAudioProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

AloopAudioProcessor::~AloopAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout AloopAudioProcessor::createParameterLayout() {
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Every range/default here is taken directly from the corresponding
    // Faust hslider/nentry declaration (dsp/effects_runtime.dsp), matching
    // docs/PARAMETER-MAP.md exactly, so a parameter at its default reproduces
    // the same DSP state a freshly-booted aloop unit starts in.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(ParamID::hpCutoff, 1), "HP Cutoff", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(ParamID::lpCutoff, 1), "LP Cutoff", 0.0f, 1.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(ParamID::lpResonance, 1), "LP Resonance", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(ParamID::reverbAmount, 1), "Reverb Amount", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(ParamID::delayAmount, 1), "Delay Amount", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(ParamID::time, 1), "Time", 0.0f, 1.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(ParamID::formant, 1), "Formant", -3.0f, 3.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(ParamID::semitones, 1), "Semitones", -12.0f, 12.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID(ParamID::microrepeatDiv, 1), "Microrepeat Div",
        juce::StringArray{ "Off", "1", "2", "4", "8", "16" }, 0));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID(ParamID::halfSpeed, 1), "Half Speed", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID(ParamID::doubleSpeed, 1), "Double Speed", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID(ParamID::clearAll, 1), "Clear All", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID(ParamID::stopAll, 1), "Stop All", false));

    return { params.begin(), params.end() };
}

void AloopAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    // Heap-allocate the Faust DSP + sampler + every scratch buffer exactly
    // once here -- NEVER inside processBlock (aloop's ADR-013: a real 320MB
    // stack-overflow SIGSEGV from stack-allocating the equivalent Faust
    // object; LooperEngine::prepare() already heap-allocates via
    // std::make_unique, matching that discipline).
    engine_.prepare(sampleRate, samplesPerBlock, &paramStore_);
    controlSurface_.setSampleRate(sampleRate);
    controlSurface_.bindAll(paramStore_);

    monoIn_.assign((size_t)samplesPerBlock, 0.0f);
    monoOut_.assign((size_t)samplesPerBlock, 0.0f);

    samplesElapsed_ = 0;
}

void AloopAudioProcessor::releaseResources() {
    // Nothing to release explicitly -- LooperEngine/ApcControlSurface own
    // their buffers via RAII (std::vector/std::unique_ptr), torn down
    // naturally on the next prepareToPlay() or destruction.
}

bool AloopAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    // Stereo in/out only (docs/ARCHITECTURE.md's "stereo-in-summed-to-mono"
    // decision) -- matches what most DAW tracks expect by default, even
    // though the DSP core itself is mono internally.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void AloopAudioProcessor::pushAutomatableParamsToStore() {
    // fx/* knobs: a straight parameter -> ParamStore mirror, exactly as if a
    // MIDI CC had set the same target (see ApcControlSurface::dispatchEvent's
    // flat CC48-55 map for the MIDI-side equivalent of these same targets).
    paramStore_.setByName("fx/hp",     apvts_.getRawParameterValue(ParamID::hpCutoff)->load());
    paramStore_.setByName("fx/lp",     apvts_.getRawParameterValue(ParamID::lpCutoff)->load());
    paramStore_.setByName("fx/lpres",  apvts_.getRawParameterValue(ParamID::lpResonance)->load());
    paramStore_.setByName("fx/reverb", apvts_.getRawParameterValue(ParamID::reverbAmount)->load());
    paramStore_.setByName("fx/delay",  apvts_.getRawParameterValue(ParamID::delayAmount)->load());
    paramStore_.setByName("fx/time",   apvts_.getRawParameterValue(ParamID::time)->load());
    paramStore_.setByName("fx/formant",apvts_.getRawParameterValue(ParamID::formant)->load());
    paramStore_.setByName("fx/pitch",  apvts_.getRawParameterValue(ParamID::semitones)->load());
    // microrepeatDiv is a choice {Off,1,2,4,8,16} -- map its index straight to
    // the divisor value (index 0 = off = 0, index 1 = "1", ... index 5 = 16),
    // matching ApcControlSurface::onMicrorepeatOn's own {1,2,4,8,16} table.
    {
        static const float divs[6] = { 0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
        int idx = (int)apvts_.getRawParameterValue(ParamID::microrepeatDiv)->load();
        if (idx < 0) idx = 0; if (idx > 5) idx = 5;
        paramStore_.setByName("fx/microrepeat_div", divs[idx]);
    }
    // cmd/* momentary commands: these have no ARM/FINISH state-machine
    // gating (unlike looperN.rec/play/erase, deliberately NOT exposed as
    // parameters that write straight into the DSP -- see
    // docs/PARAMETER-MAP.md's own note), so a direct mirror is safe.
    paramStore_.setByName("cmd/halfspeed",   apvts_.getRawParameterValue(ParamID::halfSpeed)->load()   > 0.5f ? 1.0f : 0.0f);
    paramStore_.setByName("cmd/doublespeed", apvts_.getRawParameterValue(ParamID::doubleSpeed)->load() > 0.5f ? 1.0f : 0.0f);
    paramStore_.setByName("cmd/stopall",     apvts_.getRawParameterValue(ParamID::stopAll)->load()     > 0.5f ? 1.0f : 0.0f);
    // cmd/clearall is intercepted through ApcControlSurface::onClearAll (so
    // its own shadow-state reset runs), not written here directly -- see
    // processBlock's parameter-edge-to-onClearAll bridge below.
}

void AloopAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numInCh = getTotalNumInputChannels();
    const int numOutCh = getTotalNumOutputChannels();

    // Host transport (JUCE 7+ AudioPlayHead::getPosition() API) replaces
    // aloop's LinkSnapshot reads -- see LooperEngine.cpp's own comment on
    // exactly how bpm/ppq/isPlaying substitute for Link's
    // bpm/beatPhaseMicroBeats/quantumMicroBeats.
    double hostBpm = 0.0;
    double hostPpq = 0.0;
    bool hostIsPlaying = false;
    if (auto* playHead = getPlayHead()) {
        if (auto position = playHead->getPosition()) {
            if (auto bpm = position->getBpm()) hostBpm = *bpm;
            if (auto ppq = position->getPpqPosition()) hostPpq = *ppq;
            hostIsPlaying = position->getIsPlaying();
        }
    }

    // --- MIDI dispatch: iterate the host MidiBuffer through ApcControlSurface,
    // exactly replacing aloop's ALSA rawmidi byte-at-a-time parse (midi.cpp's
    // runMidiLoop) -- JUCE has already assembled complete messages for us, so
    // there is no phase/st/d1/d2 state machine to reproduce, just a straight
    // decode into MidiEvt. now_ms is derived from the running sample count
    // (sample-accurate, no wall-clock scheduling jitter), matching the
    // monotonic clock the original used, just sourced differently.
    const double sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    for (const auto metadata : midiMessages) {
        auto msg = metadata.getMessage();
        unsigned eventNowMs = (unsigned)(((double)(samplesElapsed_ + metadata.samplePosition) / sampleRate) * 1000.0);

        MidiEvt ev{};
        bool decoded = true;
        if (msg.isNoteOn())        { ev.type = 0x90; ev.channel = msg.getChannel() - 1; ev.d1 = msg.getNoteNumber(); ev.d2 = msg.getVelocity(); }
        else if (msg.isNoteOff())  { ev.type = 0x80; ev.channel = msg.getChannel() - 1; ev.d1 = msg.getNoteNumber(); ev.d2 = 0; }
        else if (msg.isController()) { ev.type = 0xB0; ev.channel = msg.getChannel() - 1; ev.d1 = msg.getControllerNumber(); ev.d2 = msg.getControllerValue(); }
        else decoded = false;

        if (decoded) controlSurface_.dispatchEvent(ev, eventNowMs, paramStore_, &engine_);
    }
    // Block-rate hold polling (erase >=1000ms, preset capture >=1000ms) --
    // replaces the original's 100ms poll-timeout tick on a separate control
    // thread. Polling once per processBlock call is at least as fine-grained
    // as that 100ms poll for any reasonable block size.
    unsigned blockEndNowMs = (unsigned)(((double)(samplesElapsed_ + numSamples) / sampleRate) * 1000.0);
    controlSurface_.pollHolds(blockEndNowMs, paramStore_);

    // Host-automatable parameters (APVTS) mirror into the same ParamStore a
    // MIDI CC would write into -- pushed every block so automation reaches
    // the DSP identically to a live knob turn.
    pushAutomatableParamsToStore();
    // cmd/clearall needs edge detection to trigger ApcControlSurface's own
    // onClearAll (which resets its shadow state, not just the DSP zone) --
    // a plain parameter mirror (like the other cmd/* commands above) would
    // never call onClearAll at all, silently desyncing ApcControlSurface's
    // ARM/FINISH state the same way apc_grid.cpp's own history warns about
    // for a clear that bypasses ApcGrid::onClearAll.
    {
        bool clearHeld = apvts_.getRawParameterValue(ParamID::clearAll)->load() > 0.5f;
        if (clearHeld != prevClearHeld_) controlSurface_.requestClearAll(clearHeld, paramStore_);
        prevClearHeld_ = clearHeld;
    }

    // --- audio: sum L+R to mono, run the engine, duplicate back to L+R -----
    // (docs/ARCHITECTURE.md's "Decision: stereo-in-summed-to-mono" section --
    // byte-for-byte the same math audio_thread.cpp's deinterleave used,
    // just replacing "USB wire channels" with "host bus channels".)
    const float* inL = numInCh > 0 ? buffer.getReadPointer(0) : nullptr;
    const float* inR = numInCh > 1 ? buffer.getReadPointer(1) : inL;
    for (int i = 0; i < numSamples; i++) {
        float l = inL ? inL[i] : 0.0f;
        float r = inR ? inR[i] : l;
        monoIn_[(size_t)i] = (l + r) * 0.5f;
    }

    engine_.processBlock(monoIn_.data(), monoOut_.data(), numSamples, hostBpm, hostPpq, hostIsPlaying);

    for (int ch = 0; ch < numOutCh; ch++) {
        float* out = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; i++) out[i] = monoOut_[(size_t)i];
    }
    // Any extra input channels beyond stereo (shouldn't occur given
    // isBusesLayoutSupported, but defensive) are left untouched by the loop
    // above since we only ever write numOutCh channels.

    // Optional APC Key25 LED feedback: refresh once per block (ApcLeds'
    // own internal ~30Hz/2000ms-boot-delay gating means most calls are
    // no-ops), writing coalesced note-on messages into the now-cleared
    // output MidiBuffer. Harmless no-op if the host doesn't route this
    // plugin's MIDI output to a real device -- see docs/ARCHITECTURE.md.
    midiMessages.clear();
    {
        auto telemetry = engine_.snapshotTelemetry();
        unsigned nowMsForLeds = blockEndNowMs;
        apcLeds_.refresh(nowMsForLeds, controlSurface_, controlSurface_.liveEngaged(),
            [&](int note, uint8_t velocity) -> bool {
                midiMessages.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8)velocity), 0);
                return true;
            },
            telemetry.looperLevel);
    }

    samplesElapsed_ += numSamples;
}

juce::AudioProcessorEditor* AloopAudioProcessor::createEditor() {
    return new AloopAudioProcessorEditor(*this);
}

// --- session persistence ----------------------------------------------------
// Layout (all little-endian, written via juce::MemoryOutputStream which
// defaults to that byte order):
//   uint32 magic ('ALVS'), uint32 version
//   uint32 sampleRateAtSave (informational; loop lengths below are already in
//     SAMPLES at that rate, so a reload at a different host sample rate still
//     reproduces the same musical duration relative to the tempo, matching
//     how dsp/loop.dsp's ring itself is sample-indexed, not time-indexed)
//   float masterLenSamples, float recordedBpm   (cmd/master_len, cmd/recorded_bpm)
//   for each of the 20 loopers:
//     uint8 hasContent, uint8 isPlaying, uint8 isRecording
//     float lenSamples, float volume
//     uint32 ringSampleCount, then that many int16 PCM samples -- the
//       looper's actual RECORDED AUDIO, round-tripped through
//       LooperEngine::exportLooperRing/importLooperRing (see LooperEngine.h's
//       extensive derivation comment for exactly how this reaches the Faust
//       rwtable ring's real content through the DSP's own legitimate signal
//       path, and LooperEngine.cpp for the implementation this was proven
//       against). This closes what was previously a documented gap (a
//       zero-length placeholder) -- a reload now genuinely restores the
//       recorded audio, not just the on/off/length/volume state around it.
//   fx/* parameter values (float, in the same order createParameterLayout()
//     declares them) -- APVTS's own state could serialize these, but they're
//     included here too so a raw ParamStore-only consumer (or a future
//     non-APVTS host) still gets a complete session.
namespace {
    constexpr uint32_t kStateMagic = 0x414C5653;   // 'ALVS'
    constexpr uint32_t kStateVersion = 1;
}

void AloopAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::MemoryOutputStream stream(destData, false);
    stream.writeInt((int)kStateMagic);
    stream.writeInt((int)kStateVersion);
    stream.writeInt((int)getSampleRate());

    stream.writeFloat(paramStore_.get("cmd/master_len", 0.0f));
    stream.writeFloat(paramStore_.get("cmd/recorded_bpm", 0.0f));

    char z[32];
    for (int lp = 0; lp < kLooperCount; lp++) {
        snprintf(z, sizeof z, "looper%2d/rec",  lp); bool rec  = engine_.fui().get(z) > 0.5f;
        snprintf(z, sizeof z, "looper%2d/play", lp); bool play = engine_.fui().get(z) > 0.5f;
        bool hasContent = controlSurface_.looperHasContent(lp);
        stream.writeByte(hasContent ? 1 : 0);
        stream.writeByte(play ? 1 : 0);
        stream.writeByte(rec ? 1 : 0);
        snprintf(z, sizeof z, "looper%2d/len", lp); stream.writeFloat(engine_.fui().get(z, 48000.0f));
        snprintf(z, sizeof z, "looper%2d/vol", lp); stream.writeFloat(engine_.fui().get(z, 1.0f));
        // Ring-buffer content: read back the real recorded audio via
        // LooperEngine::exportLooperRing (see its own derivation comment in
        // LooperEngine.h). Only loopers with actual content are worth the
        // export sweep's extra compute() blocks; an empty/never-recorded
        // looper writes a genuine zero-length entry (correctly representing
        // "nothing was ever recorded here", not a placeholder for something
        // that couldn't be read).
        std::vector<int16_t> ring;
        if (hasContent && !rec) engine_.exportLooperRing(lp, ring);
        stream.writeInt((int)ring.size());
        if (!ring.empty()) stream.write(ring.data(), ring.size() * sizeof(int16_t));
    }

    stream.writeFloat(paramStore_.get("fx/hp", 0.0f));
    stream.writeFloat(paramStore_.get("fx/lp", 1.0f));
    stream.writeFloat(paramStore_.get("fx/lpres", 0.0f));
    stream.writeFloat(paramStore_.get("fx/reverb", 0.0f));
    stream.writeFloat(paramStore_.get("fx/delay", 0.0f));
    stream.writeFloat(paramStore_.get("fx/time", 0.5f));
    stream.writeFloat(paramStore_.get("fx/formant", 0.0f));
    stream.writeFloat(paramStore_.get("fx/pitch", 0.0f));

    // APVTS's own parameter tree, appended after our custom block so a
    // session restores host-automation-visible parameter values too (the
    // custom block above is authoritative for ApcControlSurface's shadow
    // state / looper content flags, which APVTS knows nothing about).
    if (auto xml = apvts_.copyState().createXml()) {
        juce::MemoryBlock apvtsBlock;
        juce::MemoryOutputStream apvtsStream(apvtsBlock, false);
        xml->writeTo(apvtsStream);
        apvtsStream.flush();
        stream.writeInt((int)apvtsBlock.getSize());
        stream.write(apvtsBlock.getData(), apvtsBlock.getSize());
    } else {
        stream.writeInt(0);
    }
}

void AloopAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    juce::MemoryInputStream stream(data, (size_t)sizeInBytes, false);
    uint32_t magic = (uint32_t)stream.readInt();
    if (magic != kStateMagic) return;   // not our format -- ignore rather than crash
    uint32_t version = (uint32_t)stream.readInt();
    if (version != kStateVersion) return;   // future-proofing hook; nothing to migrate yet

    stream.readInt();   // sampleRateAtSave -- informational only, loop lengths are already in samples

    float masterLen = stream.readFloat();
    float recordedBpm = stream.readFloat();
    paramStore_.setByName("cmd/master_len", masterLen);
    paramStore_.setByName("cmd/recorded_bpm", recordedBpm);
    controlSurface_.restoreMasterLenSamples((long)masterLen);

    char z[32];
    for (int lp = 0; lp < kLooperCount; lp++) {
        bool hasContent = stream.readByte() != 0;
        bool play = stream.readByte() != 0;
        bool rec  = stream.readByte() != 0;
        float len = stream.readFloat();
        float vol = stream.readFloat();
        int ringSamples = stream.readInt();
        std::vector<int16_t> ring;
        if (ringSamples > 0) {
            ring.resize((size_t)ringSamples);
            stream.read(ring.data(), (int)(ring.size() * sizeof(int16_t)));
        }

        snprintf(z, sizeof z, "looper%2d/len", lp); engine_.fui().set(z, len);
        snprintf(z, sizeof z, "looper%2d/vol", lp); engine_.fui().set(z, vol);
        // Re-record the actual audio BEFORE setting play/rec below --
        // LooperEngine::importLooperRing runs a real ARM->FINISH cycle (see
        // its derivation comment in LooperEngine.h), which itself sets rec=1
        // transiently and leaves rec=0/finishtarget=ringSamples on exit; the
        // explicit play/rec zone writes immediately after this restore the
        // caller's actually-saved on/off state on top of that, matching what
        // a live recording followed by the saved play/pause state would look
        // like.
        if (!ring.empty()) engine_.importLooperRing(lp, ring);

        snprintf(z, sizeof z, "looper%2d/play", lp); engine_.fui().set(z, play ? 1.0f : 0.0f);
        snprintf(z, sizeof z, "looper%2d/rec", lp);  engine_.fui().set(z, rec ? 1.0f : 0.0f);
        // Restore ApcControlSurface's own shadow state too -- a raw Faust
        // zone write alone would leave the NEXT pad press mis-dispatched
        // (see restoreLooperShadowState's own doc comment).
        controlSurface_.restoreLooperShadowState(lp, hasContent, play, rec);
    }

    float fxHp = stream.readFloat();
    float fxLp = stream.readFloat();
    float fxLpRes = stream.readFloat();
    float fxReverb = stream.readFloat();
    float fxDelay = stream.readFloat();
    float fxTime = stream.readFloat();
    float fxFormant = stream.readFloat();
    float fxPitch = stream.readFloat();
    paramStore_.setByName("fx/hp", fxHp);
    paramStore_.setByName("fx/lp", fxLp);
    paramStore_.setByName("fx/lpres", fxLpRes);
    paramStore_.setByName("fx/reverb", fxReverb);
    paramStore_.setByName("fx/delay", fxDelay);
    paramStore_.setByName("fx/time", fxTime);
    paramStore_.setByName("fx/formant", fxFormant);
    paramStore_.setByName("fx/pitch", fxPitch);

    int apvtsSize = stream.readInt();
    if (apvtsSize > 0) {
        juce::MemoryBlock apvtsBlock;
        stream.readIntoMemoryBlock(apvtsBlock, apvtsSize);
        if (auto xml = juce::XmlDocument::parse(apvtsBlock.toString()))
            apvts_.replaceState(juce::ValueTree::fromXml(*xml));
    }
}

} // namespace aloopvst

// JUCE's juce_audio_plugin_client module (linked in via juce_add_plugin in
// CMakeLists.txt) requires this exact free-function factory at global scope
// -- it's how the VST3/Standalone wrapper code obtains an instance of our
// AudioProcessor. Every juce_add_plugin target needs exactly one of these
// defined somewhere in its sources.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new aloopvst::AloopAudioProcessor();
}
