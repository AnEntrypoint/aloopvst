// aloop sampler — direct port of ../looper's patches/sampler.h. This class is
// intentionally UNCHANGED from the reference (it was already portable C++ with
// no bare-metal/Circle dependencies) — only the include guard and namespace
// wrapping are aloop-specific; every data structure, algorithm, and constant
// below matches looper's real hardware sampler exactly.
//
// Two capture modes, both gesture-driven from the APC Key 25:
//   * Button 65 HELD  -> record ONE shared "chromatic" sample. On release the
//     leading/trailing silence is auto-clipped and the 25 keyboard keys play
//     the sample pitched chromatically (middle C = note 60 = original speed),
//     polyphonically.
//   * Button 66 HELD  -> drum-record mode. While 66 is held, holding a keyboard
//     key records into THAT key's own drum slot (auto-clip on release). A loaded
//     drum slot plays at ORIGINAL pitch as a one-shot and OVERRIDES the
//     chromatic sample on that key.
//
// LOAD-BEARING INVARIANTS (unchanged from looper):
//   * Independent of the looper: this object touches NO loop-engine state. The
//     loopers keep recording/playing while the sampler records/plays.
//   * Sampler audio is mixed INTO the dry input buffer BEFORE the pitch/
//     effects/microrepeat/filter chain (renderInto), so samples get all
//     effects and are recordable by a loop (under SHIFT they fold into a
//     recording loop) -- see audio_thread.cpp's worker() call site.
//   * Capture reads a snapshot taken AFTER renderInto's own voice mix-in
//     (so a sample never records itself -- renderInto for THIS block always
//     runs before captureBlock for THIS block) but AFTER the SHIFT/glitch
//     loop-fold too -- DEPARTS from ../looper's reference here: user's
//     explicit request this session ("shift should route loops into the
//     sample recording... since loopers play into the input channel its a
//     surprise it doesnt already do this") means captureBlock DOES now see
//     SHIFT/glitch-folded loop content, unlike the original looper design
//     this file otherwise mirrors verbatim. See audio_thread.cpp's worker()
//     call site for the exact ordering.
//   * MIDI events arrive on the control thread; audio runs on the RT audio
//     thread. Events cross via a lock-free SPSC ring (pushEvent producer,
//     drained in renderInto consumer). Buffers are written and read only on
//     the audio thread.
//   * Click-free: per-voice attack/release gain ramps + a few-sample fade at
//     the auto-trim edges.
//
// Buffers are heap-allocated once in the ctor; no allocation in the audio
// path. Storage is s16 (short) to halve the footprint; the audio path here
// is s32 (int) mono, matching aloop's own s32-scale capture buffer.

#ifndef ALOOP_SAMPLER_H
#define ALOOP_SAMPLER_H

#include <stdint.h>
#include <string.h>
#include <math.h>

namespace aloop {

class Sampler {
public:
    static const int SR             = 48000;          // native rate
    static const int CHROM_MAX      = 5 * SR;         // 5s chromatic sample
    static const int DRUM_MAX       = 2 * SR;         // 2s per drum slot
    static const int NUM_DRUM       = 25;             // 25 keyboard keys
    static const int BASE_NOTE      = 48;             // lowest keyboard key (C2)
    static const int ROOT_NOTE      = 60;             // chromatic original-speed (C4/middle C)
    static const int VOICES         = 16;             // poly voice pool
    static const int EVENT_RING     = 64;             // control-thread -> audio-thread event ring
    static const int TRIM_THRESH    = 200;            // |s16| silence threshold
    static const int EDGE_FADE      = 64;             // trailing fade-out (samples)
    static const int PREROLL        = 32;             // samples kept before onset (preserve attack)
    static const int LEAD_DECLICK   = 8;              // tiny leading fade-in (declick only, keeps punch)

    enum EvType { EV_NONE = 0, EV_NOTE_ON, EV_NOTE_OFF,
                  EV_REC_START, EV_REC_STOP };

    Sampler()
    {
        m_chromM = new short[CHROM_MAX];
        memset(m_chromM, 0, sizeof(short) * CHROM_MAX);
        m_chromLen = 0;
        m_chromLoaded = false;

        for (int k = 0; k < NUM_DRUM; k++) {
            m_drumM[k] = new short[DRUM_MAX];
            memset(m_drumM[k], 0, sizeof(short) * DRUM_MAX);
            m_drumLen[k] = 0;
            m_drumLoaded[k] = false;
        }

        for (int v = 0; v < VOICES; v++) m_voice[v].active = false;
        m_ageCtr = 0;

        m_recActive = false;
        m_recTarget = -2;   // -2 = none, -1 = chromatic, 0..24 = drum key
        m_recPos    = 0;

        m_evHead = 0;
        m_evTail = 0;
    }

    ~Sampler()
    {
        delete[] m_chromM;
        for (int k = 0; k < NUM_DRUM; k++) { delete[] m_drumM[k]; }
    }

    // ---- Producer side (control thread) --------------------------------------
    // Lock-free SPSC push. target: REC_START uses note field as -1 (chromatic)
    // or 0..24 (drum key). NOTE_ON/OFF use note (raw MIDI note) + vel.
    void pushEvent(EvType type, int note, int vel)
    {
        unsigned head = m_evHead;
        unsigned next = (head + 1) % EVENT_RING;
        if (next == m_evTail) return;              // ring full -> drop
        m_ev[head].type = (uint8_t)type;
        m_ev[head].note = (int16_t)note;
        m_ev[head].vel  = (int16_t)vel;
        m_evHead = next;                           // publish after fields written
    }

    // Content gates read by the control thread to decide whether the keyboard
    // routes to the sampler. Cross-thread reads of plain bools — eventually-
    // consistent, which is fine for a UI routing gate.
    bool chromaticLoaded() const { return m_chromLoaded; }
    bool drumLoaded(int keyIdx) const
    {
        return (keyIdx >= 0 && keyIdx < NUM_DRUM) ? m_drumLoaded[keyIdx] : false;
    }
    static int keyIndex(int note)
    {
        return (note >= BASE_NOTE && note < BASE_NOTE + NUM_DRUM) ? (note - BASE_NOTE) : -1;
    }

    // ---- Consumer side (audio thread) ---------------------------------------
    // Append the DRY input block to the armed record buffer (no-op when not
    // recording). in is [M0..M_{n-1}] s32.
    void captureBlock(const int *in, int n)
    {
        if (!m_recActive) return;
        short *dm; int maxLen; int *lenp;
        if (!_recBuffers(dm, maxLen, lenp)) return;
        for (int i = 0; i < n; i++) {
            if (m_recPos >= maxLen) { m_recActive = false; break; }   // overrun clamp
            dm[m_recPos] = _clip16(in[i]);
            m_recPos++;
        }
        *lenp = m_recPos;
    }

    // Drain queued events, then mix all active voices into inout (s32, same
    // layout). Voices are additive; the host gates nothing — the sampler is one
    // more source in the dry input buffer.
    void renderInto(int *inout, int n)
    {
        _drainEvents();

        for (int v = 0; v < VOICES; v++) {
            Voice &vo = m_voice[v];
            if (!vo.active) continue;
            for (int i = 0; i < n; i++) {
                // End-of-sample: begin release so the tail fades click-free.
                if (vo.pos >= (double)(vo.len - 1)) { vo.target = 0.0f; }

                float sm = _readInterp(vo.M, vo.len, vo.pos);

                // Per-sample gain ramp toward target. Attack uses the per-voice
                // (duration-scaled) step so short/fast voices open in time; release
                // uses the fixed ~1ms step for a consistent click-free tail.
                if (vo.gain < vo.target)      { vo.gain += vo.attackStep; if (vo.gain > vo.target) vo.gain = vo.target; }
                else if (vo.gain > vo.target) { vo.gain -= GAIN_STEP;     if (vo.gain < vo.target) vo.gain = vo.target; }

                inout[i] += (int)(sm * vo.gain);

                vo.pos += vo.rate;
                if (vo.gain <= 0.0f && vo.target == 0.0f) { vo.active = false; break; }
            }
        }
    }

    // ---- Telemetry -----------------------------------------------------------
    bool recording() const   { return m_recActive; }
    int  recLen() const      { return m_recPos; }
    int  drumLoadedCount() const
    {
        int c = 0; for (int k = 0; k < NUM_DRUM; k++) if (m_drumLoaded[k]) c++;
        return c;
    }
    int  activeVoices() const
    {
        int c = 0; for (int v = 0; v < VOICES; v++) if (m_voice[v].active) c++;
        return c;
    }

private:
    static constexpr float GAIN_STEP = 1.0f / 48.0f;   // ~1ms attack/release @48k

    struct Voice {
        bool   active;
        const short *M;
        int    len;
        double pos;
        double rate;
        bool   sustain;     // chromatic: released by NOTE_OFF; drum one-shot: false
        int    note;        // raw MIDI note this voice answers NOTE_OFF for (-1 = none)
        float  gain;
        float  target;
        float  attackStep;  // per-voice attack ramp; scaled so short/fast (high-note) voices still reach audible gain before the sample ends
        unsigned age;
    };

    static short _clip16(int v)
    {
        return v > 32767 ? 32767 : (v < -32768 ? -32768 : (short)v);
    }

    static float _readInterp(const short *buf, int len, double pos)
    {
        int i0 = (int)pos;
        if (i0 < 0) i0 = 0;
        if (i0 >= len) return 0.0f;
        int i1 = i0 + 1; if (i1 >= len) i1 = len - 1;
        float frac = (float)(pos - (double)i0);
        return (float)buf[i0] * (1.0f - frac) + (float)buf[i1] * frac;
    }

    bool _recBuffers(short *&dm, int &maxLen, int *&lenp)
    {
        if (m_recTarget == -1) { dm = m_chromM; maxLen = CHROM_MAX; lenp = &m_chromLen; return true; }
        if (m_recTarget >= 0 && m_recTarget < NUM_DRUM) {
            dm = m_drumM[m_recTarget];
            maxLen = DRUM_MAX; lenp = &m_drumLen[m_recTarget]; return true;
        }
        return false;
    }

    void _startRecord(int target)
    {
        // target: -1 chromatic, 0..24 drum. Stop any voices reading a drum slot
        // we are about to overwrite (no read mid-rewrite).
        if (target >= 0 && target < NUM_DRUM) {
            for (int v = 0; v < VOICES; v++)
                if (m_voice[v].active && m_voice[v].M == m_drumM[target]) m_voice[v].active = false;
            m_drumLoaded[target] = false;
            m_drumLen[target] = 0;
        } else if (target == -1) {
            for (int v = 0; v < VOICES; v++)
                if (m_voice[v].active && m_voice[v].M == m_chromM) m_voice[v].active = false;
            m_chromLoaded = false;
            m_chromLen = 0;
        }
        m_recActive = true;
        m_recTarget = target;
        m_recPos    = 0;
    }

    void _stopRecord()
    {
        // Finalize whenever a capture target is pending — m_recActive may already
        // be false if captureBlock hit the overrun clamp before the stop event.
        if (m_recTarget == -2) return;
        m_recActive = false;
        short *dm; int maxLen; int *lenp;
        if (!_recBuffers(dm, maxLen, lenp)) { m_recTarget = -2; return; }
        int len = *lenp;
        int trimmed = _autoTrim(dm, len);
        *lenp = trimmed;
        if (m_recTarget == -1) m_chromLoaded = (trimmed > 0);
        else if (m_recTarget >= 0 && m_recTarget < NUM_DRUM) m_drumLoaded[m_recTarget] = (trimmed > 0);
        m_recTarget = -2;
    }

    // Auto-clip leading/trailing silence in place; returns new length. A short
    // fade-in/out is applied at the trimmed edges for click-free one-shots.
    // All-silence -> returns 0 (slot stays unloaded).
    static int _autoTrim(short *M, int len)
    {
        if (len <= 0) return 0;
        // First and last sample whose magnitude clears the silence threshold.
        int start = -1, end = -1;
        for (int i = 0; i < len; i++) {
            int a = M[i]; if (a < 0) a = -a;
            if (a > TRIM_THRESH) { if (start < 0) start = i; end = i; }
        }
        if (start < 0 || end < start) return 0;     // all silence
        // PRE-ROLL: keep a few samples BEFORE the first threshold crossing so the
        // real attack transient (drum hit / pluck) is preserved, not chopped at
        // the steep part of its rise. Without this the onset starts mid-transient
        // and a leading fade would further soften the punch.
        if (start > PREROLL) start -= PREROLL; else start = 0;
        int newLen = end - start + 1;
        if (start > 0) {
            memmove(M, M + start, sizeof(short) * newLen);
        }
        // Leading edge: only a TINY declick (preserve the attack), not a long
        // fade-in. Trailing edge: a longer fade-out so one-shots end click-free.
        int fin = newLen < LEAD_DECLICK ? newLen : LEAD_DECLICK;
        for (int i = 0; i < fin; i++) {
            float g = (float)i / (float)fin;
            M[i] = (short)(M[i] * g);
        }
        int fout = newLen < EDGE_FADE ? newLen : EDGE_FADE;
        for (int i = 0; i < fout; i++) {
            float g = (float)i / (float)fout;
            int j = newLen - 1 - i;
            M[j] = (short)(M[j] * g);
        }
        return newLen;
    }

    void _spawnVoice(const short *M, int len, double rate, bool sustain, int note)
    {
        if (len <= 0) return;
        int slot = -1; unsigned oldest = 0xFFFFFFFF;
        for (int v = 0; v < VOICES; v++) {
            if (!m_voice[v].active) { slot = v; break; }
            if (m_voice[v].age < oldest) { oldest = m_voice[v].age; slot = v; }
        }
        Voice &vo = m_voice[slot];
        vo.active = true; vo.M = M; vo.len = len;
        vo.pos = 0.0; vo.rate = rate; vo.sustain = sustain; vo.note = note;
        vo.gain = 0.0f; vo.target = 1.0f; vo.age = ++m_ageCtr;
        // Attack ramp scaled to how long the voice will actually play. A fast
        // (high-note) voice covers the sample in len/rate output samples; with the
        // default ~1ms (48-sample) attack it would release before the gain opened
        // -> SILENT (the "high notes don't play" bug). Reach full gain within the
        // first quarter of the playable length (but never slower than the default
        // ~1ms, and never instant -> declick).
        double playable = (rate > 0.0) ? ((double)len / rate) : (double)len;
        float fastStep = (playable > 4.0) ? (float)(1.0 / (playable * 0.25)) : 0.25f;
        vo.attackStep = fastStep > GAIN_STEP ? fastStep : GAIN_STEP;
    }

    void _noteOn(int note, int /*vel*/)
    {
        int k = keyIndex(note);
        if (k >= 0 && m_drumLoaded[k]) {
            // Drum one-shot at original pitch (ignores note-off, plays to end).
            _spawnVoice(m_drumM[k], m_drumLen[k], 1.0, false, -1);
            return;
        }
        if (m_chromLoaded) {
            // Mono-per-note retrigger: release any voice already sustaining THIS
            // note so a re-press doesn't stack voices and so the 16-voice steal
            // can't orphan a held note's eventual NOTE_OFF (auto-sustain bug).
            for (int v = 0; v < VOICES; v++)
                if (m_voice[v].active && m_voice[v].sustain && m_voice[v].note == note)
                    m_voice[v].target = 0.0f;
            double rate = pow(2.0, (double)(note - ROOT_NOTE) / 12.0);
            _spawnVoice(m_chromM, m_chromLen, rate, true, note);
        }
    }

    void _noteOff(int note)
    {
        // Release sustaining (chromatic) voices owned by this note.
        for (int v = 0; v < VOICES; v++)
            if (m_voice[v].active && m_voice[v].sustain && m_voice[v].note == note)
                m_voice[v].target = 0.0f;
    }

    void _drainEvents()
    {
        while (m_evTail != m_evHead) {
            uint8_t type = m_ev[m_evTail].type;
            int     note = m_ev[m_evTail].note;
            int     vel  = m_ev[m_evTail].vel;
            m_evTail = (m_evTail + 1) % EVENT_RING;
            switch (type) {
                case EV_NOTE_ON:   _noteOn(note, vel);  break;
                case EV_NOTE_OFF:  _noteOff(note);      break;
                case EV_REC_START: _startRecord(note);  break;   // note field = target
                case EV_REC_STOP:  _stopRecord();       break;
                default: break;
            }
        }
    }

    // Chromatic sample
    short *m_chromM;
    int    m_chromLen;
    volatile bool m_chromLoaded;

    // Per-key drum slots
    short *m_drumM[NUM_DRUM];
    int    m_drumLen[NUM_DRUM];
    volatile bool m_drumLoaded[NUM_DRUM];

    // Voice pool
    Voice    m_voice[VOICES];
    unsigned m_ageCtr;

    // Record state (audio thread only)
    volatile bool m_recActive;
    int  m_recTarget;   // -2 none, -1 chromatic, 0..24 drum
    int  m_recPos;

    // control-thread -> audio-thread event ring
    struct Ev { uint8_t type; int16_t note; int16_t vel; };
    volatile Ev m_ev[EVENT_RING];
    volatile unsigned m_evHead, m_evTail;
};

} // namespace aloop
#endif // ALOOP_SAMPLER_H
