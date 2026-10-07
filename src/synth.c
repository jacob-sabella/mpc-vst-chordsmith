#include "synth.h"
#include <math.h>
#include <string.h>

#define SR 44100.0f
#define TABLE 1024
#define ATTACK (1.0f / (0.003f * SR))     /* 3 ms linear attack */
#define SUSTAIN 0.35f

static float SINE[TABLE + 1];
static float DECAY, BRIGHT_DECAY, RELEASE;   /* per-sample multipliers */

static void init_tables(void) {
    if (SINE[TABLE / 4] > 0.5f) return;   /* already built (sin(pi/2) = 1) */
    for (int i = 0; i <= TABLE; i++) SINE[i] = sinf(2.0f * (float)M_PI * i / TABLE);
    DECAY = expf(-1.0f / (0.8f * SR));         /* toward sustain, 0.8 s time constant */
    BRIGHT_DECAY = expf(-1.0f / (0.25f * SR)); /* the harmonics fade faster than the note */
    RELEASE = expf(-1.0f / (0.08f * SR));    /* about half a second to -60 dB */
}

static inline float sine(float ph) {   /* ph in cycles, 0..1 (wraps) */
    ph -= (float)(int)ph;
    float x = ph * TABLE;
    int i = (int)x;
    return SINE[i] + (SINE[i + 1] - SINE[i]) * (x - i);
}

void synth_reset(synth_t *s) {
    init_tables();
    memset(s, 0, sizeof *s);
}

void synth_note(synth_t *s, int note, int vel) {
    if (vel <= 0) {
        for (int i = 0; i < SYNTH_VOICES; i++)
            if (s->v[i].stage && s->v[i].stage != 3 && s->v[i].note == note) s->v[i].stage = 3;
        return;
    }
    /* the same note again retriggers its voice; else a free voice; else the quietest releasing one; else the
     * oldest. A taken voice keeps its phase and level and glides from there (the attack starts at its level),
     * so stealing never jumps the waveform: a jump to zero clicked. */
    int pick = -1;
    for (int i = 0; i < SYNTH_VOICES && pick < 0; i++) if (s->v[i].stage && s->v[i].note == note) pick = i;
    for (int i = 0; i < SYNTH_VOICES && pick < 0; i++) if (!s->v[i].stage) pick = i;
    for (int i = 0; i < SYNTH_VOICES; i++)
        if (s->v[i].stage == 3 && (pick < 0 || (s->v[pick].stage == 3 && s->v[i].env < s->v[pick].env))) pick = i;
    if (pick < 0) {
        pick = 0;
        for (int i = 1; i < SYNTH_VOICES; i++) if (s->age[i] < s->age[pick]) pick = i;
    }
    synth_voice_t *v = &s->v[pick];
    if (!v->stage) { v->ph = 0; v->env = 0; }
    v->note = note;
    v->stage = 1;
    v->inc = 440.0f * powf(2.0f, (note - 69) / 12.0f) / SR;
    v->bright = 1.0f;
    v->gain = 0.10f * (0.25f + 0.75f * vel / 127.0f);   /* a 5-note chord at full velocity peaks near 0.5 */
    s->age[pick] = ++s->clock;
}

void synth_all_off(synth_t *s) {
    for (int i = 0; i < SYNTH_VOICES; i++) if (s->v[i].stage) s->v[i].stage = 3;
}

void synth_render(synth_t *s, float *out, int frames, float level) {
    for (int i = 0; i < SYNTH_VOICES; i++) {
        synth_voice_t *v = &s->v[i];
        if (!v->stage) continue;
        float g = v->gain * level;
        for (int f = 0; f < frames; f++) {
            if (v->stage == 1) { if ((v->env += ATTACK) >= 1.0f) { v->env = 1.0f; v->stage = 2; } }
            else if (v->stage == 2) v->env = SUSTAIN + (v->env - SUSTAIN) * DECAY;
            else if ((v->env *= RELEASE) < 1e-3f) { v->stage = 0; v->env = 0; break; }
            if ((v->bright *= BRIGHT_DECAY) < 1e-5f) v->bright = 0;   /* no denormals */
            float x = sine(v->ph) + v->bright * (0.35f * sine(2.0f * v->ph) + 0.12f * sine(3.0f * v->ph));
            if ((v->ph += v->inc) >= 1.0f) v->ph -= 1.0f;
            x *= g * v->env;
            out[2 * f] += x;
            out[2 * f + 1] += x;
        }
    }
}
