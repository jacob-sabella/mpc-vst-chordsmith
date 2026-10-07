/* Chordsmith: pads or keys in, chords out. One note on the plugin's track plays a chord slot, built
 * from the key and scale (or a chord set), voiced, optionally strummed, and sent to the ALSA port
 * (seq_out.c) for another track to play; with INPUT = BUTTONS the chord is built from a root and the chord
 * buttons held instead. With the preview synth on, a small synth (synth.c) also plays every note on the
 * plugin's own track, to try things without routing the port.
 *
 * Threads: set_param/get_param run on MPC's UI thread, midi/render on the audio thread. Parameters
 * are plain ints read once per event; screen taps are handed over in an atomic bit mask; the
 * "last chord" text is double-buffered. */
#include "theory.h"
#include "seq_out.h"
#include "synth.h"
#include "engine.h"   /* copied from mpc-vst-plugins wrapper/ into vst/build/ by vst/build.sh */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define TAP_MS 600            /* a screen tap holds its chord this long */
#define QUIET_MS 500          /* ignore slot "taps" this long after create/state load (host restoring values) */
#define NUM_IDS (128 + NUM_SLOTS)   /* input notes, then one id per screen slot */
#define ECHO_MS 250           /* our own note coming back this soon on the keys' own channel is an echo, not a key (see midi()) */
#define LATE_MS 2000          /* on any other channel, this late still: MPC under load returned echoes after 400 ms and more */
#define LIM_CEIL 0.89f        /* the preview synth's limiter: peaks held at -1 dBFS */
#define LIM_RELEASE (1.0f / (0.12f * SR))
#define MAX_OUT 32            /* notes one held chord can send (a harp runs a chord up four octaves) */
/* PERFORM: how a held chord plays. CHORD = all together (spread by STRUM), SLOP = each note late by a random
 * part of STRUM, ARP = one note at a time at RATE, HARP = up four octaves at a quarter of RATE, PATTERN = the
 * whole chord on the hits of a 16-step pattern at RATE. */
enum { PF_CHORD, PF_SLOP, PF_ARP_UP, PF_ARP_DOWN, PF_ARP_UPDOWN, PF_HARP, PF_PAT1, PF_PAT2, PF_PAT3, NUM_PERFORM };
static const char *const PATTERNS[3] = {"x..x..x...x.x...", "x.x.x.x.x.x.x.x.", "x..x..x.x..x..x."};
enum { NUM_RATES = 6 };
static const float RATE_BEATS[NUM_RATES] = {1.0f, 0.5f, 1.0f / 3, 0.25f, 1.0f / 6, 0.125f};   /* 1/4 1/8 1/8T 1/16 1/16T 1/32 */
/* the panic tap shares the slot tap mask above the slots */
enum { TAP_PANIC = 31 };

enum {
    P_KEY, P_SCALE, P_SOURCE, P_SET, P_EXT, P_VOICING, P_INVERSION, P_OCTAVE, P_VOICE_LEAD, P_BASS,
    P_STRUM, P_STRUM_DIR, P_VEL_MODE, P_VELOCITY, P_INPUT, P_PAD_BASE, P_CHANNEL,
    P_KEY_MAP, P_SPLIT, P_SPLIT_NOTE, P_SOUND, P_LEVEL,
    P_CTYPE, P_X6, P_XM7, P_XMAJ7, P_X9, P_PAD_BTNS, P_LATCH, P_VDIAL, P_PERFORM, P_RATE, P_BASS_CH,
    P_EDIT, P_CROOT, P_CQUAL = P_CROOT + NUM_SLOTS, NUM_P = P_CQUAL + NUM_SLOTS   /* CUSTOM: root and quality per slot */
};
static const struct { const char *key; int min, max, def; } PDEF[NUM_P] = {
    {"key", 0, 11, 0}, {"scale", 0, NUM_SCALES - 1, SCALE_MAJOR}, {"source", 0, 2, 0},
    {"set", 0, 255, 0}, {"extension", 0, NUM_EXTS - 1, EXT_7TH}, {"voicing", 0, NUM_VOICINGS - 1, VOICE_CLOSE},
    {"inversion", 0, 3, 0}, {"octave", 0, 4, 2}, {"voice_lead", 0, 1, 1}, {"bass", 0, 1, 0},
    {"strum", 0, 100, 0}, {"strum_dir", 0, 1, 0}, {"vel_mode", 0, 1, 0}, {"velocity", 1, 127, 100},
    {"input", 0, 2, 0}, {"pad_base", 0, 120, 36}, {"channel", 1, 16, 2},
    {"key_map", 0, 1, 0}, {"split", 0, 1, 0}, {"split_note", 0, 127, 60},
    {"sound", 0, 1, 1}, {"synth_level", 0, 100, 70},
    {"chord_type", 0, NUM_BTYPES - 1, BT_AUTO}, {"ext_6", 0, 1, 0}, {"ext_m7", 0, 1, 0}, {"ext_maj7", 0, 1, 0},
    {"ext_9", 0, 1, 0}, {"pad_buttons", 0, 1, 1}, {"latch", 0, 1, 0}, {"voice_dial", -12, 12, 0},
    {"perform", 0, NUM_PERFORM - 1, PF_CHORD}, {"rate", 0, NUM_RATES - 1, 3}, {"bass_ch", 0, 16, 0},
    {"edit", 0, NUM_SLOTS - 1, 0},   /* the slot the ROOT / CHORD steppers edit (a tile tap picks it) */
    {"cust_root_1", 0, 11, 0}, {"cust_root_2", 0, 11, 0}, {"cust_root_3", 0, 11, 0}, {"cust_root_4", 0, 11, 0},
    {"cust_root_5", 0, 11, 0}, {"cust_root_6", 0, 11, 0}, {"cust_root_7", 0, 11, 0}, {"cust_root_8", 0, 11, 0},
    {"cust_qual_1", 0, 127, 0}, {"cust_qual_2", 0, 127, 0}, {"cust_qual_3", 0, 127, 0}, {"cust_qual_4", 0, 127, 0},
    {"cust_qual_5", 0, 127, 0}, {"cust_qual_6", 0, 127, 0}, {"cust_qual_7", 0, 127, 0}, {"cust_qual_8", 0, 127, 0},
};
enum { SRC_SCALE, SRC_SET, SRC_CUSTOM };
enum { TRAIL_N = 8, TRAIL_LEN = 12 };
enum { IN_PADS, IN_KEYS, IN_BUTTONS };
enum { MAP_WHITE, MAP_SCALE };

typedef struct {
    int active, slot, vel, n;
    int note[MAX_OUT];
    unsigned char ch[MAX_OUT];   /* channel each note goes out on (the bass may have its own) */
    long due[MAX_OUT];        /* frame each note-on goes out (strum, slop, harp) */
    unsigned sent;            /* bit i: note i is sounding */
    long release_at;          /* screen taps only; 0 = held until note-off */
    int latched;              /* key let go with LATCH on: sounds until the next chord */
    int perform, step;        /* PF_*; arp/pattern step counter */
    long step_len, next_step; /* frames per arp/pattern step, frame of the next one */
    int root_kind, root;      /* button chords: 1 = key (root = the note), 2 = pad (root = slot), for re-chording */
} held_t;

typedef struct {
    int p[NUM_P];
    atomic_uint taps;         /* bit i: slot i tapped on screen */
    long now, quiet_until;
    held_t held[NUM_IDS];
    unsigned char ref[16][128];
    voicing_t prev;
    char text[2][96];
    atomic_int text_idx;
    int port_ok;
    signed char thru_ch[128];    /* keys above the split: channel a passed-through note went out on, -1 = not held */
    /* echo filter, per channel and note: how many note-ons / note-offs we sent that may still come back,
     * and when the last of each went out (see midi()) */
    unsigned char pend_on[16][128], pend_off[16][128];
    long on_at[16][128], off_at[16][128];
    synth_t synth;
    float mix[256];
    float lim;                   /* the preview synth's limiter gain (render()) */
    /* button chords: pad buttons held (audio thread) */
    unsigned btn_types;          /* bit t: type button t (BT_DIM..BT_SUS) held */
    int btn_last;                /* the type button pressed last that is still held, 0 = none */
    int btn_ext;                 /* BX_* bits of the extension pads held */
    char qbuf[32];               /* name of a button chord the quality table lacks */
    float bpm;                   /* host tempo, for arp/pattern/harp rates */
    unsigned rng;
    int in_ch;                   /* channel the last key/pad came in on */
    int echo_seen;               /* our port's notes come back (it is enabled as a track input) */
    int last_src;                /* SCALE or SET, whichever showed last: what COPY takes into CUSTOM */
    char trail[TRAIL_N][TRAIL_LEN];   /* the last chords played, oldest first (the PLAYED line) */
    int trail_n;
    int echo_clash;              /* our port echoed back on the keys' own channel: MPC merges those notes */
} inst_t;

static const char *const NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void source_chord(const inst_t *s, int src, int slot, chord_t *c) {
    if (src == SRC_CUSTOM) chord_from_quality(s->p[P_CROOT + slot], s->p[P_CQUAL + slot], c);
    else if (src == SRC_SET) set_chord(s->p[P_KEY], clampi(s->p[P_SET], 0, NUM_CHORD_SETS - 1), slot, c);
    else if (slot < 7) diatonic_chord(s->p[P_KEY], s->p[P_SCALE], slot, s->p[P_EXT], c);
    else {   /* slot 8 in scale mode: the tonic an octave up, so a full I..vii + I fits the pads */
        diatonic_chord(s->p[P_KEY], s->p[P_SCALE], 0, s->p[P_EXT], c);
        for (int i = 0; i < c->n; i++) c->iv[i] += 12;
    }
}

static void slot_chord(const inst_t *s, int slot, chord_t *c) { source_chord(s, s->p[P_SOURCE], slot, c); }

static int flats(const inst_t *s) { return key_uses_flats(s->p[P_KEY], s->p[P_SOURCE] == SRC_SCALE ? s->p[P_SCALE] : SCALE_MAJOR); }

#define ECHO_FRAMES ((long)ECHO_MS * SR / 1000)
#define LATE_FRAMES ((long)LATE_MS * SR / 1000)

/* every note out goes through here, so the echo filter knows what may come back */
static void send(inst_t *s, int ch, int n, int vel) {
    unsigned char *pend = vel ? s->pend_on[ch] : s->pend_off[ch];
    long *at = vel ? s->on_at[ch] : s->off_at[ch];
    if (s->now - at[n] > LATE_FRAMES) pend[n] = 0;   /* the earlier ones never came back (port not enabled) */
    if (pend[n] < 255) pend[n]++;
    at[n] = s->now;
    seq_out_note(ch, n, vel);
}

/* an incoming note that is our own coming back (same channel and note, moments later): swallow it, once per
 * note we sent. The channel keeps a key played on another one from being taken for an echo. How late an echo
 * can be: on a channel the keys don't use, up to LATE_MS, since nothing else arrives there (a late echo let
 * through played as a key: a quick tap's echoes came back after its release, each started a chord, and those
 * echoed again, a pile of chords that clipped and stalled MPC); on the keys' own channel only ECHO_MS, so a key
 * repeating a note we just sent is not held back long. */
static int echoed(inst_t *s, int ch, int n, int on) {
    unsigned char *pend = on ? s->pend_on[ch] : s->pend_off[ch];
    long *at = on ? s->on_at[ch] : s->off_at[ch];
    if (!pend[n] || s->now - at[n] > (ch == s->in_ch ? ECHO_FRAMES : LATE_FRAMES)) return 0;
    /* on the keys' own channel (MIDI OUT set to it) a key and an echo look alike: there an echoed on must
     * still be sounding, and a held key let go is a key whatever we sent */
    if (ch == s->in_ch && (on ? !s->ref[ch][n] : s->held[n].active)) return 0;
    pend[n]--;
    s->echo_seen = 1;
    return 1;
}

/* MIDI OUT on the keys' channel, with a chord that holds the key's own note: on the device the echo of that
 * note never arrives (MPC merges it with the held key) and neither does the key's note-off, so the chord
 * hangs. Once the chord's other echoes are back and that one is still missing, say so on screen. */
static void check_clash(inst_t *s) {
    if (s->echo_clash || !s->echo_seen) return;
    int ch = s->in_ch;
    for (int n = 0; n < 128; n++)
        if (s->pend_on[ch][n] && s->ref[ch][n] && s->held[n].active && !s->held[n].release_at &&
            s->now - s->on_at[ch][n] > ECHO_FRAMES) { s->echo_clash = 1; return; }
}

static void note_on(inst_t *s, int ch, int n, int vel) {
    if (s->ref[ch][n]) send(s, ch, n, 0);   /* retrigger a note two chords share */
    if (s->ref[ch][n] < 255) s->ref[ch][n]++;
    send(s, ch, n, vel);
    if (s->p[P_SOUND]) synth_note(&s->synth, n, vel);
}

static void note_off(inst_t *s, int ch, int n) {
    if (s->ref[ch][n] && --s->ref[ch][n] == 0) {
        send(s, ch, n, 0);
        synth_note(&s->synth, n, 0);
    }
}

static void release(inst_t *s, int id) {
    held_t *h = &s->held[id];
    if (!h->active) return;
    for (int i = 0; i < h->n; i++)
        if (h->sent & (1u << i)) note_off(s, h->ch[i], h->note[i]);
    h->active = 0;
    h->latched = 0;
}

/* fl: spelling, -1 = the key's */
static void set_text(inst_t *s, const chord_t *c, const voicing_t *v, int fl) {
    int w = 1 - atomic_load(&s->text_idx), len;
    if (fl < 0) fl = flats(s);
    char *t = s->text[w];
    chord_name(c, fl, t, sizeof s->text[0]);
    len = (int)strlen(t);
    for (int i = 0; i < v->n && len < (int)sizeof s->text[0] - 8; i++)
        len += snprintf(t + len, sizeof s->text[0] - len, "%s%s%d", i ? " " : "  ",
                        fl ? pc_name(v->note[i] % 12, 1) : NAMES[v->note[i] % 12], v->note[i] / 12 - 1);
    atomic_store(&s->text_idx, w);
    /* the PLAYED line: the chord joins the trail unless it is the one there already (a re-chord, a repeat) */
    char nm[TRAIL_LEN];
    chord_name(c, fl, nm, sizeof nm);
    if (s->trail_n && !strcmp(s->trail[s->trail_n - 1], nm)) return;
    if (s->trail_n == TRAIL_N) { memmove(s->trail[0], s->trail[1], (TRAIL_N - 1) * TRAIL_LEN); s->trail_n--; }
    memcpy(s->trail[s->trail_n++], nm, TRAIL_LEN);
}

/* CUSTOM: take the chords SCALE or SET last showed, as a starting point to edit */
static void copy_chords(inst_t *s) {
    for (int i = 0; i < NUM_SLOTS; i++) {
        chord_t c;
        source_chord(s, s->last_src, i, &c);
        int q = quality_of(&c);
        if (q < 0) continue;
        s->p[P_CROOT + i] = c.root;
        s->p[P_CQUAL + i] = q;
    }
    int w = 1 - atomic_load(&s->text_idx);
    if (s->last_src == SRC_SET) snprintf(s->text[w], sizeof s->text[0], "COPIED: %s", CHORD_SETS[clampi(s->p[P_SET], 0, NUM_CHORD_SETS - 1)].name);
    else snprintf(s->text[w], sizeof s->text[0], "COPIED: %s %s", pc_name(s->p[P_KEY], flats(s)), SCALE_NAMES[s->p[P_SCALE]]);
    atomic_store(&s->text_idx, w);
}

static unsigned rnd(inst_t *s) { s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5; return s->rng; }

static void sort_notes(int *n, int count) {
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && n[j] < n[j - 1]; j--) { int t = n[j]; n[j] = n[j - 1]; n[j - 1] = t; }
}

/* VOICING dial: each step up moves the lowest chord note up an octave, each step down the highest down one,
 * so turning it walks the chord through its inversions (the bass note stays put) */
static void dial(voicing_t *v, int steps, int bass) {
    int lo = bass && v->n > 1 ? 1 : 0;
    for (; steps > 0; steps--) {
        if (v->note[lo] + 12 > 127) break;
        v->note[lo] += 12;
        sort_notes(v->note + lo, v->n - lo);
    }
    for (; steps < 0; steps++) {
        int top = v->n - 1;
        if (v->note[top] - 12 < 0 || (lo && v->note[top] - 12 <= v->note[0])) break;
        v->note[top] -= 12;
        sort_notes(v->note + lo, v->n - lo);
    }
}

static long step_frames(const inst_t *s) {
    float bpm = s->bpm >= 20 && s->bpm <= 400 ? s->bpm : 120;
    return (long)(SR * 60.0f / bpm * RATE_BEATS[clampi(s->p[P_RATE], 0, NUM_RATES - 1)]);
}

/* LATCH: a new chord ends the latched one */
static void release_latched(inst_t *s, int except) {
    for (int id = 0; id < NUM_IDS; id++)
        if (id != except && s->held[id].active && s->held[id].latched) release(s, id);
}

/* id: the input note (or 128 + slot for a tap); slot: the screen tile to light (-1: none);
 * shift: octaves added after voicing (upper pad rows, keys); fl: spelling, -1 = the key's */
static void play_chord(inst_t *s, int id, int slot, const chord_t *c, int shift, int vel, long release_at, int fl) {
    voicing_t v;
    int kind = s->held[id].root_kind, root = s->held[id].root;   /* kept across a re-chord */
    release(s, id);
    if (c->n <= 0) return;
    voice_opts_t o = {s->p[P_VOICING], s->p[P_INVERSION], s->p[P_OCTAVE] - 2, s->p[P_BASS], s->p[P_VOICE_LEAD]};
    voice_chord(c, &o, &s->prev, &v);
    s->prev = v;
    dial(&v, s->p[P_VDIAL], s->p[P_BASS]);
    set_text(s, c, &v, fl);
    held_t *h = &s->held[id];
    memset(h, 0, sizeof *h);
    h->active = 1;
    h->slot = slot;
    h->root_kind = kind;
    h->root = root;
    int ch = clampi(s->p[P_CHANNEL], 1, 16) - 1, bass_ch = s->p[P_BASS_CH] ? s->p[P_BASS_CH] - 1 : ch;
    h->vel = s->p[P_VEL_MODE] == 1 ? s->p[P_VELOCITY] : clampi(vel, 1, 127);
    h->release_at = release_at;
    h->perform = clampi(s->p[P_PERFORM], 0, NUM_PERFORM - 1);
    int octaves = h->perform == PF_HARP ? 4 : 1;
    for (int o8 = 0; o8 < octaves; o8++)
        for (int i = 0; i < v.n && h->n < MAX_OUT; i++) {
            if (o8 && s->p[P_BASS] && i == 0 && v.n > 1) continue;   /* the harp runs the chord, not the bass */
            int n = v.note[i] + 12 * (shift + o8);
            if (n < 0 || n > 127) continue;
            h->ch[h->n] = (unsigned char)(s->p[P_BASS] && i == 0 && v.n > 1 ? bass_ch : ch);
            h->note[h->n++] = n;
        }
    long gap = (long)s->p[P_STRUM] * SR / 1000, step = step_frames(s);
    h->step_len = step;
    h->next_step = s->now;
    for (int i = 0; i < h->n; i++) {
        switch (h->perform) {
        case PF_SLOP: h->due[i] = s->now + (long)(rnd(s) % (unsigned)((gap ? gap : (long)40 * SR / 1000) + 1)); break;
        case PF_HARP: h->due[i] = s->now + step / 4 * i; break;
        default:      /* strum: up = lowest note first, down = highest first */
            h->due[i] = s->now + gap * (s->p[P_STRUM_DIR] ? h->n - 1 - i : i);
        }
    }
}

/* ARP: one note at a time, a step apart; the last one sounds until the next */
static void arp_step(inst_t *s, held_t *h, long until) {
    while (h->n > 0 && h->next_step < until) {
        int len = h->perform == PF_ARP_UPDOWN && h->n > 2 ? 2 * h->n - 2 : h->n, k = h->step % len;
        int i = h->perform == PF_ARP_DOWN ? h->n - 1 - k : k < h->n ? k : 2 * h->n - 2 - k;
        for (int j = 0; j < h->n; j++)
            if (h->sent & (1u << j)) { note_off(s, h->ch[j], h->note[j]); h->sent &= ~(1u << j); }
        note_on(s, h->ch[i], h->note[i], h->vel);
        h->sent |= 1u << i;
        h->step++;
        h->next_step += h->step_len;
    }
}

/* PATTERN: the whole chord on each hit, off on the step after it unless that is a hit too */
static void pattern_step(inst_t *s, held_t *h, long until) {
    const char *pat = PATTERNS[h->perform - PF_PAT1];
    while (h->next_step < until) {
        int hit = pat[h->step % 16] == 'x';
        if (hit || h->sent)
            for (int j = 0; j < h->n; j++)
                if (h->sent & (1u << j)) note_off(s, h->ch[j], h->note[j]);
        h->sent = 0;
        if (hit)
            for (int j = 0; j < h->n; j++) { note_on(s, h->ch[j], h->note[j], h->vel); h->sent |= 1u << j; }
        h->step++;
        h->next_step += h->step_len;
    }
}

static void play(inst_t *s, int id, int slot, int shift, int vel, long release_at) {
    chord_t c;
    s->held[id].root_kind = 0;
    slot_chord(s, slot, &c);
    play_chord(s, id, slot, &c, shift, vel, release_at, -1);
}

/* A keyboard key (INPUT = KEYS). White Keys: C..B play slots 1..7 of whatever is loaded, a black key the
 * white key to its left with more colour. Scale Notes: every key plays the chord built on it in KEY/SCALE
 * (borrowed chords off the scale). Either way the octave follows the key played, C4 = as set. */
static void play_key(inst_t *s, int note, int vel) {
    static const int WHITE[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    static const int BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    /* the octave follows the key, but only one either way of OCTAVE, so low keys don't turn to mud */
    int pc = note % 12, shift = clampi(note / 12 - 5, -1, 1), slot;
    chord_t c, v;
    s->held[note].root_kind = 0;
    if (s->p[P_KEY_MAP] == MAP_SCALE) {
        int fl, diatonic = key_chord(s->p[P_KEY], s->p[P_SCALE], pc, s->p[P_EXT], &c, &fl);
        slot = diatonic && s->p[P_SOURCE] == SRC_SCALE ? scale_degree(s->p[P_KEY], s->p[P_SCALE], pc) : -1;
        play_chord(s, note, slot, &c, shift, vel, 0, fl);
        return;
    }
    slot = WHITE[pc];
    slot_chord(s, slot, &c);
    if (BLACK[pc]) {
        vary_chord(&c, s->p[P_KEY], s->p[P_SOURCE] == SRC_SCALE ? s->p[P_SCALE] : SCALE_MAJOR, &v);
        c = v;
    }
    play_chord(s, note, slot, &c, shift, vel, 0, -1);
}

/* ---- button chords (INPUT = BUTTONS) ----
 * Pads 1-4 are extension buttons (6, m7, maj7, 9), pads 5-8 chord type buttons (dim, m, maj, sus4), held
 * together like chord buttons on a keyboard instrument. Pads 9-16 play roots I..vii and I up an octave of
 * KEY/SCALE; every key (any note outside the pad range) plays its own root. With no type button held the
 * screen's CHORD TYPE applies (AUTO: the chord the key/scale gives that root), and the screen's extension
 * switches add to the held ones. Changing buttons while a root is held re-chords it. */
static void button_play(inst_t *s, int id, int vel) {
    held_t *h = &s->held[id];
    int pc, shift, slot = -1, fl;
    if (h->root_kind == 2) {   /* a root pad: scale degree, the eighth an octave up */
        chord_t d;
        diatonic_chord(s->p[P_KEY], s->p[P_SCALE], h->root % 7, EXT_TRIAD, &d);
        pc = d.root;
        shift = h->root == 7;
        slot = h->root;
    } else {
        pc = h->root % 12;
        shift = clampi(h->root / 12 - 5, -1, 1);
    }
    int type = s->btn_last ? s->btn_last : s->p[P_CTYPE];
    int ext = s->btn_ext | (s->p[P_X6] ? BX_6 : 0) | (s->p[P_XM7] ? BX_M7 : 0) | (s->p[P_XMAJ7] ? BX_MAJ7 : 0) |
              (s->p[P_X9] ? BX_9 : 0);
    chord_t c;
    button_chord(s->p[P_KEY], s->p[P_SCALE], pc, type, ext, &c, s->qbuf, sizeof s->qbuf, &fl);
    play_chord(s, id, slot, &c, shift, vel, 0, fl);
}

static void rechord(inst_t *s) {
    for (int id = 0; id < 128; id++)
        if (s->held[id].active && s->held[id].root_kind) {
            int latched = s->held[id].latched;
            button_play(s, id, s->held[id].vel);
            s->held[id].latched = latched;
        }
}

static void button(inst_t *s, int b, int on) {
    if (b < 4) {
        if (on) s->btn_ext |= 1 << b; else s->btn_ext &= ~(1 << b);
    } else {
        int t = BT_DIM + b - 4;
        if (on) { s->btn_types |= 1u << t; s->btn_last = t; }
        else {
            s->btn_types &= ~(1u << t);
            if (s->btn_last == t) {   /* back to another type still held, else none */
                s->btn_last = 0;
                for (int k = BT_DIM; k < NUM_BTYPES; k++) if (s->btn_types & (1u << k)) s->btn_last = k;
            }
        }
    }
    rechord(s);
}

static void flush(inst_t *s, long until) {
    for (int id = 0; id < NUM_IDS; id++) {
        held_t *h = &s->held[id];
        if (!h->active) continue;
        if (h->perform >= PF_ARP_UP && h->perform <= PF_ARP_UPDOWN) { arp_step(s, h, until); continue; }
        if (h->perform >= PF_PAT1) { pattern_step(s, h, until); continue; }
        for (int i = 0; i < h->n; i++)
            if (!(h->sent & (1u << i)) && h->due[i] < until) {
                note_on(s, h->ch[i], h->note[i], h->vel);
                h->sent |= 1u << i;
            }
    }
}

/* keys above the split point pass straight through, a melody over the chords */
static void thru_note(inst_t *s, int note, int vel) {
    if (vel > 0) {
        if (s->thru_ch[note] >= 0) note_off(s, s->thru_ch[note], note);
        s->thru_ch[note] = (signed char)(clampi(s->p[P_CHANNEL], 1, 16) - 1);
        note_on(s, s->thru_ch[note], note, s->p[P_VEL_MODE] == 1 ? s->p[P_VELOCITY] : vel);
    } else if (s->thru_ch[note] >= 0) {
        note_off(s, s->thru_ch[note], note);
        s->thru_ch[note] = -1;
    }
}

static void *create(const char *dir) {
    (void)dir;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    for (int i = 0; i < NUM_P; i++) s->p[i] = PDEF[i].def;
    s->port_ok = seq_out_acquire();
    s->quiet_until = (long)QUIET_MS * SR / 1000;
    memset(s->thru_ch, -1, sizeof s->thru_ch);
    s->bpm = 120;
    s->rng = 0x9e3779b9u;
    s->in_ch = -1;
    s->last_src = SRC_SCALE;
    copy_chords(s);   /* CUSTOM starts as the C major 7ths, not eight C majors */
    snprintf(s->text[0], sizeof s->text[0], "PLAY A PAD, OR TAP A CHORD");
    atomic_store(&s->text_idx, 0);
    synth_reset(&s->synth);
    s->lim = 1.0f;
    return s;
}

static void panic(inst_t *s) {
    for (int id = 0; id < NUM_IDS; id++) s->held[id].active = 0;
    memset(s->thru_ch, -1, sizeof s->thru_ch);
    s->btn_types = 0;
    s->btn_last = s->btn_ext = 0;
    s->echo_clash = 0;
    s->echo_seen = 0;
    synth_all_off(&s->synth);
    for (int ch = 0; ch < 16; ch++)
        for (int n = 0; n < 128; n++)
            if (s->ref[ch][n]) { s->ref[ch][n] = 0; send(s, ch, n, 0); }
}

static void destroy(void *inst) {
    inst_t *s = inst;
    panic(s);
    seq_out_release();
    free(s);
}

static void midi(void *inst, const uint8_t *msg, int len) {
    inst_t *s = inst;
    if (len < 3) return;
    int st = msg[0] & 0xf0, note = msg[1] & 127, vel = msg[2] & 127;
    if (st == 0xb0 && (note == 123 || note == 120)) { panic(s); return; }   /* all notes / sound off */
    if (st != 0x90 && st != 0x80) return;
    int on = st == 0x90 && vel > 0;
    /* MPC enables new MIDI ports for track input by default (MidiDevices.AutoEnableForTracks), so our own
     * port can feed straight back into this track: every chord note would come back as a key and play more
     * chords. Each note-on and note-off we sent comes back once, moments later: swallow exactly those. Note-ons
     * and note-offs are counted apart, because a re-chord sends a note's off and its next on together and
     * the off must not get through as a key let go. */
    if (echoed(s, msg[0] & 15, note, on)) {
        /* if it comes back on the channel the keys use, MPC merges it with a key held on the same note and
         * that key's note-off never arrives: the chord would hang. Say so on screen. */
        if (on && (msg[0] & 15) == s->in_ch && s->held[note].active && !s->held[note].release_at) s->echo_clash = 1;
        return;
    }
    /* keys above the split point pass through (a melody over the chords) */
    int thru = s->p[P_INPUT] == IN_KEYS && s->p[P_SPLIT] && note >= s->p[P_SPLIT_NOTE];
    if (thru || s->thru_ch[note] >= 0) {   /* also a note held across a mode switch */
        thru_note(s, note, on ? vel : 0);
        if (thru) return;
    }
    int d = note - s->p[P_PAD_BASE];
    if (s->p[P_INPUT] == IN_BUTTONS && s->p[P_PAD_BTNS] && d >= 0 && d < NUM_SLOTS) {
        button(s, d, on);
        return;
    }
    if (!on) {
        if (s->p[P_LATCH] && s->held[note].active) s->held[note].latched = 1;
        else release(s, note);
        return;
    }
    s->in_ch = msg[0] & 15;
    release_latched(s, -1);
    if (s->p[P_INPUT] == IN_BUTTONS) {
        held_t *h = &s->held[note];
        h->root_kind = s->p[P_PAD_BTNS] && d >= NUM_SLOTS && d < 2 * NUM_SLOTS ? 2 : 1;
        h->root = h->root_kind == 2 ? d - NUM_SLOTS : note;
        button_play(s, note, vel);
        return;
    }
    if (s->p[P_INPUT] == IN_KEYS) {
        play_key(s, note, vel);
        return;
    }
    if (d < 0 || d >= 2 * NUM_SLOTS) return;
    play(s, note, d % NUM_SLOTS, d / NUM_SLOTS, vel, 0);   /* pads 9-16: the same chords an octave up */
}

static void render(void *inst, int16_t *out, int frames) {
    inst_t *s = inst;
    unsigned taps = atomic_exchange(&s->taps, 0);
    if (taps & (1u << TAP_PANIC)) panic(s);
    if (!s->p[P_LATCH]) release_latched(s, -1);
    for (int i = 0; i < NUM_SLOTS; i++)
        if ((taps & (1u << i)) && s->now >= s->quiet_until) {
            release_latched(s, -1);
            play(s, 128 + i, i, 0, 100, s->now + (long)TAP_MS * SR / 1000);
        }
    flush(s, s->now + frames);
    for (int i = 0; i < NUM_SLOTS; i++) {
        held_t *h = &s->held[128 + i];
        if (h->active && h->release_at && s->now >= h->release_at) release(s, 128 + i);
    }
    check_clash(s);
    s->now += frames;
    memset(out, 0, frames * 2 * sizeof *out);
    if (!s->p[P_SOUND]) { synth_all_off(&s->synth); return; }
    for (int done = 0; done < frames;) {
        int n = frames - done > 128 ? 128 : frames - done;
        memset(s->mix, 0, sizeof s->mix);
        synth_render(&s->synth, s->mix, n, s->p[P_LEVEL] / 100.0f);
        /* a limiter, not a clipper: many chords at once turn down together instead of distorting. The gain drops
         * at once to keep a frame under LIM_CEIL and comes back over about 120 ms. */
        for (int f = 0; f < n; f++) {
            float l = s->mix[2 * f], r = s->mix[2 * f + 1], a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            s->lim += (1.0f - s->lim) * LIM_RELEASE;
            if (a * s->lim > LIM_CEIL) s->lim = LIM_CEIL / a;
            l *= s->lim; r *= s->lim;
            out[2 * (done + f)] = (int16_t)lrintf((l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l) * 32767.0f);
            out[2 * (done + f) + 1] = (int16_t)lrintf((r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r) * 32767.0f);
        }
        done += n;
    }
}

static int find_param(const char *key) {
    for (int i = 0; i < NUM_P; i++)
        if (!strcmp(PDEF[i].key, key)) return i;
    return -1;
}

static int slot_index(const char *key, const char *suffix) {
    int n = 0, used = 0;
    if (sscanf(key, "slot_%d%n", &n, &used) != 1 || strcmp(key + used, suffix)) return -1;
    return n >= 1 && n <= NUM_SLOTS ? n - 1 : -1;
}

static void set_param(void *inst, const char *key, const char *val);

static void load_state(inst_t *s, const char *st) {
    char buf[1024], *save = NULL;
    snprintf(buf, sizeof buf, "%s", st);
    for (char *kv = strtok_r(buf, ";", &save); kv; kv = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        if (find_param(kv) >= 0) set_param(s, kv, eq + 1);
    }
    s->quiet_until = s->now + (long)QUIET_MS * SR / 1000;
}

static void set_param(void *inst, const char *key, const char *val) {
    inst_t *s = inst;
    int i = find_param(key), sl;
    if (i >= 0) s->p[i] = clampi((int)lround(atof(val)), PDEF[i].min, PDEF[i].max);
    else if ((sl = slot_index(key, "")) >= 0) {
        if (atof(val) > 0.5) atomic_fetch_or(&s->taps, 1u << sl);
        if (s->p[P_SOURCE] == SRC_CUSTOM && s->now >= s->quiet_until) s->p[P_EDIT] = sl;   /* a tap picks the slot to edit */
    }
    else if (!strcmp(key, "edit_root")) s->p[P_CROOT + s->p[P_EDIT]] = clampi((int)lround(atof(val)), 0, 11);
    else if (!strcmp(key, "edit_qual")) s->p[P_CQUAL + s->p[P_EDIT]] = clampi((int)lround(atof(val)), 0, quality_count() - 1);
    else if (!strcmp(key, "copy")) { if (atof(val) > 0.5) copy_chords(s); }
    else if (!strcmp(key, "panic")) { if (atof(val) > 0.5) atomic_fetch_or(&s->taps, 1u << TAP_PANIC); }
    else if (!strcmp(key, "state")) load_state(s, val);
    else if (!strcmp(key, "lfo_bpm")) s->bpm = (float)atof(val);   /* the host tempo (vst.json HAS_LFO_BPM) */
    if (i == P_CHANNEL) s->echo_clash = 0;
    if (i == P_SET) s->p[P_SET] = clampi(s->p[P_SET], 0, NUM_CHORD_SETS - 1);
    if (i == P_SOURCE && s->p[P_SOURCE] != SRC_CUSTOM) s->last_src = s->p[P_SOURCE];
    if (i >= P_CQUAL && i < P_CQUAL + NUM_SLOTS) s->p[i] = clampi(s->p[i], 0, quality_count() - 1);
}

static int get_param(void *inst, const char *key, char *buf, int len) {
    inst_t *s = inst;
    int i = find_param(key), sl;
    if (i >= 0) return snprintf(buf, len, "%d", s->p[i]) + 1;
    if ((sl = slot_index(key, "")) >= 0) {   /* a tile: "3  Em7", the key letter instead on the white keys */
        static const char *const WHITE[NUM_SLOTS] = {"C", "D", "E", "F", "G", "A", "B", "8"};
        chord_t c;
        char nm[32], num[4];
        slot_chord(s, sl, &c);
        chord_name(&c, flats(s), nm, sizeof nm);
        snprintf(num, sizeof num, "%d", sl + 1);
        return snprintf(buf, len, "%s  %s%s", s->p[P_INPUT] == IN_KEYS && s->p[P_KEY_MAP] == MAP_WHITE ? WHITE[sl] : num,
                        nm, s->p[P_SOURCE] == SRC_CUSTOM && s->p[P_EDIT] == sl ? "   < EDIT" : "") + 1;
    }
    if (!strcmp(key, "edit_root")) return snprintf(buf, len, "%d", s->p[P_CROOT + s->p[P_EDIT]]) + 1;
    if (!strcmp(key, "edit_qual")) return snprintf(buf, len, "%d", s->p[P_CQUAL + s->p[P_EDIT]]) + 1;
    if (!strcmp(key, "edit_name")) return snprintf(buf, len, "PAD %d", s->p[P_EDIT] + 1) + 1;
    if (!strcmp(key, "edit_root_name")) return snprintf(buf, len, "%s", pc_name(s->p[P_CROOT + s->p[P_EDIT]], flats(s))) + 1;
    if (!strcmp(key, "edit_qual_name")) {
        const char *q = quality_name(s->p[P_CQUAL + s->p[P_EDIT]]);
        return snprintf(buf, len, "%s", q && *q ? q : "maj") + 1;
    }
    if (!strcmp(key, "copy")) return snprintf(buf, len, "0") + 1;
    if (!strcmp(key, "trail")) {   /* "PLAYED: Cmaj7 > Am7 > ...": the newest that fit in the readout's 47 characters */
        int from = s->trail_n, used = 8;
        while (from > 0 && used + (int)strlen(s->trail[from - 1]) + 3 <= 47) used += (int)strlen(s->trail[--from]) + 3;
        int n = snprintf(buf, len, "PLAYED: ");
        if (from == s->trail_n) return snprintf(buf + n, len - n, "-") + n + 1;
        for (int i = from; i < s->trail_n && n < len; i++) n += snprintf(buf + n, len - n, "%s%s", i > from ? " > " : "", s->trail[i]);
        return n + 1;
    }
    if ((sl = slot_index(key, "_on")) >= 0) {
        int on = 0;
        for (int id = 0; id < NUM_IDS && !on; id++) on = s->held[id].active && s->held[id].slot == sl;
        return snprintf(buf, len, "%d", on) + 1;
    }
    if (!strcmp(key, "set_name")) {
        if (s->p[P_SOURCE] == SRC_SET) return snprintf(buf, len, "%s", CHORD_SETS[clampi(s->p[P_SET], 0, NUM_CHORD_SETS - 1)].name) + 1;
        return snprintf(buf, len, "%s %s", pc_name(s->p[P_KEY], flats(s)), SCALE_NAMES[s->p[P_SCALE]]) + 1;
    }
    if (!strcmp(key, "chord")) return snprintf(buf, len, "%s", s->text[atomic_load(&s->text_idx)]) + 1;
    if (!strcmp(key, "split_name")) {
            int n = s->p[P_SPLIT_NOTE];
        return snprintf(buf, len, "%s%d", flats(s) ? pc_name(n % 12, 1) : NAMES[n % 12], n / 12 - 1) + 1;
    }
    if (!strcmp(key, "status"))
        return snprintf(buf, len, "%s", s->echo_clash ? "SAME CH AS KEYS: CHANGE"
                                                      : seq_out_status()) + 1;
    if (!strcmp(key, "bass_ch_name"))
        return (s->p[P_BASS_CH] ? snprintf(buf, len, "%d", s->p[P_BASS_CH]) : snprintf(buf, len, "SAME")) + 1;
    if (!strcmp(key, "panic")) return snprintf(buf, len, "0") + 1;
    if (!strcmp(key, "output")) {   /* where the chords go (PLAY tab); the wrapper shows up to 47 characters */
        int ch = clampi(s->p[P_CHANNEL], 1, 16);
        if (s->echo_clash) return snprintf(buf, len, "MIDI CH %d IS THE KEYS' CHANNEL: CHANGE IT", ch) + 1;
        if (!s->port_ok) return snprintf(buf, len, "%s", s->p[P_SOUND] ? "PREVIEW SYNTH ONLY (NO MIDI PORT)"
                                                                       : "SILENT: NO MIDI PORT, PREVIEW SYNTH OFF") + 1;
        if (s->p[P_SOUND]) return snprintf(buf, len, "PREVIEW SYNTH + MIDI OUT CH %d", ch) + 1;
        return snprintf(buf, len, "MIDI OUT CH %d ONLY (PREVIEW SYNTH OFF)", ch) + 1;
    }
    if (!strcmp(key, "buttons_hint")) {
        if (s->p[P_INPUT] != IN_BUTTONS) return snprintf(buf, len, "SET PLAY FROM TO BUTTONS TO USE THESE") + 1;
        return snprintf(buf, len, "%s", s->p[P_PAD_BTNS] ? "PADS 1-8 = TYPES, PADS 9-16 + KEYS = ROOTS"
                                                        : "PICK A TYPE, PLAY ROOTS ON PADS OR KEYS") + 1;
    }
    if (!strcmp(key, "state")) {
        int n = 0;
        for (int k = 0; k < NUM_P && n < len; k++) n += snprintf(buf + n, len - n, "%s=%d;", PDEF[k].key, s->p[k]);
        return n < len ? n + 1 : 0;
    }
    return 0;
}

static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, NULL};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
