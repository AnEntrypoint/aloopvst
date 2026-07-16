// aloopvst control store — a near-verbatim port of aloop's src/control/midi.h
// ParamStore struct (see docs/CLONE-PARITY-REFERENCE.md). Name-keyed atomic
// control store bridging MIDI dispatch (ApcControlSurface) and the Faust zone
// writes (LooperEngine::processBlock). No ALSA dependency in the original, so
// nothing here changes except the removal of the (ALSA-only) runMidiLoop
// declaration that used to sit alongside it in aloop's midi.h — the plugin's
// MIDI dispatch is ApcControlSurface, driven per-block from a JUCE MidiBuffer
// instead of a blocking ALSA rawmidi read loop.
#pragma once

#include <atomic>
#include <array>
#include <string>
#include <unordered_map>
#include <mutex>

namespace aloopvst {

// Name-keyed control store. Targets are the control names the map binds to
// ("looper3/rec", "fx/hp", "cmd/clearall", …). The MIDI dispatch writes values
// by name; processBlock reads by name to set the matching Faust control zone.
//
// The set of names is fixed at startup (bind() during ApcControlSurface's
// prepareToPlay-time bindAll() call), so the value array never resizes at
// runtime — reads/writes are plain atomics indexed by a slot resolved once.
// This keeps the audio-thread read lock-free, exactly like aloop's original.
struct ParamStore {
    static constexpr int MAX = 256;
    std::array<std::atomic<float>, MAX> value;
    // name -> slot index, populated by bind() at startup (before processBlock
    // reads). After startup this map is read-only, so name->slot lookup is
    // safe without additional synchronization on the read side.
    std::unordered_map<std::string, int> slot;
    std::mutex bindMtx;            // guards slot during startup binding only
    int count = 0;

    ParamStore() { for (auto& v : value) v.store(0.0f); }

    // Register a target name -> a slot (idempotent). Called during
    // ApcControlSurface::bindAll(), which must run once, before processBlock
    // ever reads. `defaultVal` seeds the slot's initial value — CRITICAL for
    // any target whose Faust zone has a non-zero compiled-in default (e.g.
    // fx/lp's LPCUT defaults to 1.0 = fully open, fx/time's TIME defaults to
    // 0.5). See aloop's midi.h ParamStore::bind doc comment for the exact bug
    // class this guards against (a bound target silently forced to 0.0 by
    // the very first processBlock call, before any control event ever
    // touches it — e.g. fx/lp forcing LPCUT to 0.0 = total silence).
    void bind(const std::string& name, float defaultVal = 0.0f) {
        std::lock_guard<std::mutex> g(bindMtx);
        if (slot.find(name) == slot.end() && count < MAX) {
            int idx = count++;
            slot[name] = idx;
            value[idx].store(defaultVal, std::memory_order_relaxed);
        }
    }
    // Write a value by target name (MIDI dispatch, called from processBlock's
    // MidiBuffer iteration). No-op if unbound.
    void setByName(const std::string& name, float v) {
        auto it = slot.find(name);
        if (it != slot.end()) value[it->second].store(v, std::memory_order_relaxed);
    }
    // Read a value by target name (the DSP orchestration side). Returns def
    // if unbound.
    float get(const std::string& name, float def = 0.0f) const {
        auto it = slot.find(name);
        return it != slot.end() ? value[it->second].load(std::memory_order_relaxed) : def;
    }
    // Iterate bound names (processBlock uses this once per block to map
    // names -> Faust zones via FaustUI).
    template <typename F> void forEach(F&& f) const { for (auto& kv : slot) f(kv.first, kv.second); }
};

} // namespace aloopvst
