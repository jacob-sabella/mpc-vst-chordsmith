/* A small preview synth, so Chordsmith can be heard on its own track without routing its MIDI port:
 * 24 voices of a sine plus two fading harmonics (a soft electric-piano tone). Audio thread only. */
#pragma once
#include <stdint.h>

#define SYNTH_VOICES 24   /* four 5-note chords and a strum tail without stealing */

typedef struct {
    int note, stage;          /* stage: 0 off, 1 attack, 2 decay/sustain, 3 release */
    float ph, inc, env, bright, gain;
} synth_voice_t;

typedef struct {
    synth_voice_t v[SYNTH_VOICES];
    unsigned age[SYNTH_VOICES], clock;
} synth_t;

void synth_reset(synth_t *s);
void synth_note(synth_t *s, int note, int vel);   /* vel 0 = release */
void synth_all_off(synth_t *s);                   /* release every voice */
/* Adds into out_lr (interleaved stereo float) at level 0..1. */
void synth_render(synth_t *s, float *out_lr, int frames, float level);
