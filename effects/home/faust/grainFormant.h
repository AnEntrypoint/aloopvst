// grainFormant.h — a -12 octaver WITH independent formant via grain
// playback-speed (research-backed; prototype proto-grain-formant.cpp proven).
// Reads the DRY input with THREE independent rates: output-epoch spacing =
// Tin/scale (sets -12 pitch), input-epoch advance = Tin per emission (consumes
// input at `scale`), grain CONTENT read = fm (formant). Overlapping Hann
// 2-period grains = click-free. fm==1 => formants ride with pitch (natural -12).
//
// STREAMING gap-bound (the part the offline prototype didn't need): the input
// epoch lags the writer by the -12 lag (advances ~scale/sample). Left
// unbounded it walks off the ring; so when the epoch drifts past a target lag
// it RESPLICES — jumps forward by a WHOLE number of input periods (pitch-
// neutral, like the main engine's splice) to restore the target lag. Whole-
// period jumps keep phase => no pitch error; the Hann overlap hides the splice.
// Bare-metal C headers; no allocation in the audio path.

#ifndef GRAIN_FORMANT_H
#define GRAIN_FORMANT_H
#include <math.h>
#include <string.h>

class GrainFormant {
public:
    static const int RBUF = 16384;
    static const int VOICES = 6;

    void reset() {
        memset(m_ring, 0, sizeof(m_ring));
        m_wr = 0; m_Tin = 256.0; m_scale = 0.5f;
        m_fm = 1.0f; m_targetFm = 1.0f;
        m_inEpoch = 0.0; m_sinceEmit = 1e9; m_seeded = false;
        for (int v = 0; v < VOICES; v++) m_v[v].active = false;
        m_nextV = 0;
    }
    GrainFormant() { reset(); }

    void setScale(float s)        { if (s > 0.05f && s < 4.0f) m_scale = s; }
    void setInputPeriod(double p) { if (p >= 32.0 && p <= 2048.0) m_Tin = p; }
    void setFormantFactor(float f){ if (f<0.5f) f=0.5f; if (f>2.0f) f=2.0f; m_targetFm = f; }

    float factorNow() const { return m_fm; }
    float targetFactorNow() const { return m_targetFm; }
    double periodNow() const { return m_Tin; }
    float scaleNow() const { return m_scale; }
    inline void write(float dry) { m_ring[m_wr & (RBUF - 1)] = dry; m_wr++; }
    inline float process(float dry) { write(dry); return read(); }

    inline float read() {
        m_fm += (m_targetFm - m_fm) * (1.0f / 480.0f);
        double Tin   = m_Tin;
        double outHop = Tin / (double)m_scale;       // -12 => 2*Tin
        int    glen   = (int)(2.0 * Tin);
        // Target lag of the input epoch behind the writer: enough that the
        // grain's forward content read (center + (glen/2)*fm) stays < writer.
        double targetLag = Tin * (3.0 + (double)m_fm);

        if (!m_seeded) {
            m_inEpoch = (double)m_wr - targetLag;
            m_seeded = true; m_sinceEmit = outHop;
        }

        if (m_sinceEmit >= outHop) {
            m_sinceEmit -= outHop;
            // Gap-bound RESPLICE: if the epoch has drifted too far behind the
            // writer (input piling up) or too close (about to read the future),
            // snap it back onto the target lag by a WHOLE number of periods so
            // phase — hence pitch — is preserved.
            // Resplice deadband widened 2*Tin -> 5*Tin. Each snap jumps the
            // grain source to a DIFFERENT moment of past audio; on real input
            // the period estimate jitters so frequent snaps make the texture
            // "jump in and out of different moments" / sound like two voices.
            // A wider band makes snaps rare (only on genuine large drift), so
            // the grain stream stays on one continuous stretch of audio far
            // longer = smoother. Extra lag drift is a few ms, inaudible.
            double lag = (double)m_wr - m_inEpoch;
            if (lag > targetLag + Tin * 5.0 || lag < targetLag - Tin * 5.0) {
                double want = (double)m_wr - targetLag;
                double diff = want - m_inEpoch;
                double nper = floor(diff / Tin + 0.5);   // whole periods
                m_inEpoch += nper * Tin;                  // phase-preserving snap
            }
            Voice& nv = m_v[m_nextV];
            nv.active = true; nv.k = 0; nv.len = glen; nv.center = m_inEpoch;
            m_nextV = (m_nextV + 1) % VOICES;
            m_inEpoch += Tin;                             // advance one period
        }
        m_sinceEmit += 1.0;

        float acc = 0.0f;
        for (int vi = 0; vi < VOICES; vi++) {
            Voice& v = m_v[vi];
            if (!v.active) continue;
            float ph = (float)v.k / (float)v.len;
            float win = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * ph);
            double src = v.center + (double)(v.k - v.len / 2) * (double)m_fm;
            acc += win * readRing(src);
            v.k++;
            if (v.k >= v.len) v.active = false;
        }
        return acc;
    }

private:
    struct Voice { bool active; int k; int len; double center; };
    float  m_ring[RBUF];
    unsigned m_wr;
    double m_Tin, m_inEpoch, m_sinceEmit;
    float  m_scale, m_fm, m_targetFm;
    bool   m_seeded;
    Voice  m_v[VOICES];
    int    m_nextV;

    inline float readRing(double pos) const {
        double maxPos = (double)m_wr - 1.0;
        double minPos = (double)m_wr - (double)(RBUF - 2);
        if (pos > maxPos) pos = maxPos;
        if (pos < minPos) pos = minPos;
        int i = (int)pos; if (pos < 0) i = (int)pos - 1;
        double fr = pos - (double)i;
        float a = m_ring[(unsigned)i & (RBUF - 1)];
        float b = m_ring[(unsigned)(i + 1) & (RBUF - 1)];
        return a * (float)(1.0 - fr) + b * (float)fr;
    }
};
#endif
