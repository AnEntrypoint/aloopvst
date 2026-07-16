// The Faust param-binding UI shim — identical in structure to aloop's own
// audio_thread.cpp FaustUI (that file is the authoritative reference; see
// docs/CLONE-PARITY-REFERENCE.md). Captures each Faust control's full
// group-path label -> its live zone pointer, so the plugin can push/read
// values into the generated AloopLoopDsp exactly as the hardware shell does.
#pragma once
#include <map>
#include <string>
#include <vector>

#define FAUSTFLOAT float

struct FaustMeta { void declare(const char*, const char*) {} };

struct FaustUI {
    std::map<std::string, float*> zones;
    std::vector<std::string> path;

    std::string full(const char* label) const {
        std::string p;
        for (auto& g : path) if (!g.empty()) { p += g; p += "/"; }
        p += label;
        return p;
    }
    void openTabBox(const char* l) { path.push_back(l ? l : ""); }
    void openHorizontalBox(const char* l) { path.push_back(l ? l : ""); }
    void openVerticalBox(const char* l) { path.push_back(l ? l : ""); }
    void closeBox() { if (!path.empty()) path.pop_back(); }
    void addButton(const char* l, float* z) { zones[full(l)] = z; }
    void addCheckButton(const char* l, float* z) { zones[full(l)] = z; }
    void addVerticalSlider(const char* l, float* z, float, float, float, float) { zones[full(l)] = z; }
    void addHorizontalSlider(const char* l, float* z, float, float, float, float) { zones[full(l)] = z; }
    void addNumEntry(const char* l, float* z, float, float, float, float) { zones[full(l)] = z; }
    // Bargraphs (Faust hbargraph/vbargraph -- read-only telemetry OUTPUTS,
    // e.g. dsp/loop.dsp's "level" and "writeidx" zones) are still plain
    // float* zones under the hood, exactly like a slider's input zone --
    // the only difference is direction (Faust writes them, the host reads
    // them), not representation. WITNESSED live (this session, via a
    // standalone diagnostic harness that isolated LooperEngine from the
    // GUI): leaving these as no-ops meant fui.get("looperN/writeidx") and
    // fui.get("looperN/level") ALWAYS returned the caller's default (0.0),
    // silently breaking EngineTelemetry::looperWriteIdx (used by
    // ApcControlSurface's finish-quantization for every SUBSEQUENT
    // recording after the first -- the first recording doesn't consult
    // writeIdx at all, which is why this specific gap didn't explain the
    // very-first-recording symptom this session was actually chasing) and
    // EngineTelemetry::looperLevel (the on-screen level meter, PluginEditor.cpp's
    // LooperCell::paint). This exact no-op stub is inherited unchanged from
    // aloop's own audio_thread.cpp FaustUI shim -- a pre-existing defect in
    // the shared design this port faithfully replicated, not a regression
    // introduced here; fixed properly rather than carried forward silently.
    void addHorizontalBargraph(const char* l, float* z, float, float) { zones[full(l)] = z; }
    void addVerticalBargraph(const char* l, float* z, float, float) { zones[full(l)] = z; }
    void addSoundfile(const char*, const char*, void**) {}
    void declare(float*, const char*, const char*) {}

    // Matches the exact path first, else any zone whose path ENDS with the
    // given suffix (so "HPCUT" finds ".../HPCUT" and "looper03/rec" matches
    // exactly) -- same matching rule as aloop's own shim.
    void set(const char* name, float v) {
        auto it = zones.find(name);
        if (it != zones.end()) { *it->second = v; return; }
        std::string suf(name);
        for (auto& kv : zones) {
            const std::string& k = kv.first;
            if (k.size() >= suf.size() && k.compare(k.size() - suf.size(), suf.size(), suf) == 0) { *kv.second = v; return; }
        }
    }
    float get(const char* name, float def = 0.0f) const {
        auto it = zones.find(name);
        if (it != zones.end()) return *it->second;
        std::string suf(name);
        for (auto& kv : zones) {
            const std::string& k = kv.first;
            if (k.size() >= suf.size() && k.compare(k.size() - suf.size(), suf.size(), suf) == 0) return *kv.second;
        }
        return def;
    }
};

#define Meta FaustMeta
#define UI FaustUI
#define dsp FaustDspBase
struct FaustDspBase { virtual ~FaustDspBase() {} };
#include "loop.cpp"
#undef dsp
