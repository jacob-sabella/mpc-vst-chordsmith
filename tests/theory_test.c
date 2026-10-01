/* Offline checks of the chord theory and of the engine end to end (MIDI in -> notes out through a
 * capture sink). Built and run by vst/test.sh under ASan/UBSan. Exit 1 on failure. */
#include "theory.h"
#include "seq_out.h"
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#include <stddef.h>
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void name_of(int key, int scale, int deg, int ext, char *buf) {
    chord_t c;
    diatonic_chord(key, scale, deg, ext, &c);
    chord_name(&c, key_uses_flats(key, scale), buf, 32);
}

static void expect_row(int key, int scale, int ext, const char *want) {
    char got[256] = "", b[32];
    for (int d = 0; d < 7; d++) { name_of(key, scale, d, ext, b); strcat(got, d ? " " : ""); strcat(got, b); }
    CHECK(!strcmp(got, want), "%s %s ext %d: got '%s', want '%s'", pc_name(key, 0), SCALE_NAMES[scale], ext, got, want);
}

static void expect_keys(int key, int scale, int ext, const char *want) {
    char got[256] = "", name[32];
    chord_t c;
    for (int pc = 0; pc < 12; pc++) {
        int fl;
        key_chord(key, scale, (key + pc) % 12, ext, &c, &fl);
        chord_name(&c, fl, name, sizeof name);
        strcat(got, pc ? " " : ""); strcat(got, name);
    }
    CHECK(!strcmp(got, want), "keys %d/%d/%d: got '%s' want '%s'", key, scale, ext, got, want);
}

static void expect_vary(int key, int scale, int ext, const char *want) {
    char got[256] = "", name[32];
    chord_t c, v;
    for (int d = 0; d < 7; d++) {
        diatonic_chord(key, scale, d, ext, &c);
        vary_chord(&c, key, scale, &v);
        chord_name(&v, 0, name, sizeof name);
        strcat(got, d ? " " : ""); strcat(got, name);
    }
    CHECK(!strcmp(got, want), "vary %d/%d/%d: got '%s' want '%s'", key, scale, ext, got, want);
}

static void test_theory(void) {
    expect_row(0, SCALE_MAJOR, EXT_TRIAD, "C Dm Em F G Am Bdim");
    expect_row(0, SCALE_MAJOR, EXT_7TH, "Cmaj7 Dm7 Em7 Fmaj7 G7 Am7 Bm7b5");
    expect_row(0, SCALE_MAJOR, EXT_9TH, "Cmaj9 Dm9 Em7b9 Fmaj9 G9 Am9 Bm7b5b9");
    expect_row(9, SCALE_MINOR, EXT_7TH, "Am7 Bm7b5 Cmaj7 Dm7 Em7 Fmaj7 G7");
    expect_row(9, SCALE_HARM_MINOR, EXT_7TH, "AmMaj7 Bm7b5 Cmaj7#5 Dm7 E7 Fmaj7 G#dim7");
    expect_row(9, SCALE_HARM_MINOR, EXT_9TH, "AmMaj9 Bm7b5b9 Cmaj9#5 Dm9 E7b9 Fmaj7#9 G#dim7b9");
    expect_row(5, SCALE_MAJOR, EXT_TRIAD, "F Gm Am Bb C Dm Edim");
    expect_row(2, SCALE_MINOR, EXT_TRIAD, "Dm Edim F Gm Am Bb C");
    /* keys: every pitch class in C major and A minor; off the scale, borrowed chords */
    expect_keys(0, SCALE_MAJOR, EXT_TRIAD, "C Db Dm Eb Em F F#dim G Ab Am Bb Bdim");
    expect_keys(0, SCALE_MAJOR, EXT_7TH, "Cmaj7 Dbmaj7 Dm7 Ebmaj7 Em7 Fmaj7 F#dim7 G7 Abmaj7 Am7 Bb7 Bm7b5");
    expect_keys(9, SCALE_MINOR, EXT_TRIAD, "Am Bb Bdim C C#m Dm D#dim Em F F#m G G#dim");
    /* black-key colour: triad -> 7th from the scale, 7th -> 9th, 9th -> triad */
    expect_vary(0, SCALE_MAJOR, EXT_TRIAD, "Cmaj7 Dm7 Em7 Fmaj7 G7 Am7 Bm7b5");
    expect_vary(0, SCALE_MAJOR, EXT_7TH, "Cmaj9 Dm9 Em7b9 Fmaj9 G9 Am9 Bm7b5b9");
    expect_vary(0, SCALE_MAJOR, EXT_9TH, "C Dm Em F G Am Bdim");
    /* sus4 takes its b7 even off the scale; F and B's diatonic "sus4" (an augmented/diminished 4th) has no name */
    expect_vary(0, SCALE_MAJOR, EXT_SUS4, "C7sus4 D7sus4 E7sus4 F? G7sus4 A7sus4 B?");

    /* every scale, degree, key and extension must produce a chord the quality table names */
    for (int k = 0; k < 12; k++)
        for (int s = 0; s < NUM_SCALES; s++)
            for (int e = 0; e < NUM_EXTS; e++)
                for (int d = 0; d < 7; d++) {
                    chord_t c;
                    diatonic_chord(k, s, d, e, &c);
                    if (e != EXT_SUS2 && e != EXT_SUS4)
                        CHECK(strcmp(c.quality, "?"), "unnamed chord: key %d %s degree %d ext %d", k, SCALE_NAMES[s], d, e);
                    CHECK(c.n >= 3 && c.iv[0] == 0, "bad chord shape");
                }

    /* every set: 1..8 chords, every token parses */
    for (int s = 0; s < NUM_CHORD_SETS; s++) {
        int n = 0;
        const char *p = CHORD_SETS[s].chords;
        for (const char *q = p; *q; q++) if (*q != ' ' && (q == p || q[-1] == ' ')) n++;
        CHECK(n >= 1 && n <= NUM_SLOTS, "set %s has %d chords", CHORD_SETS[s].name, n);
        for (int i = 0; i < n; i++) {
            chord_t c;
            CHECK(set_chord(0, s, i, &c) && c.n >= 3, "set %s chord %d does not parse", CHORD_SETS[s].name, i + 1);
        }
        chord_t c;
        CHECK(!set_chord(0, s, n, &c) && c.n == 0, "set %s: slot past the end is not empty", CHORD_SETS[s].name);
    }
    chord_t c;
    char b[32];
    set_chord(9, 10, 0, &c); chord_name(&c, 0, b, sizeof b); CHECK(!strcmp(b, "Am"), "Minor Pop in A slot 1: %s", b);
    set_chord(9, 10, 1, &c); chord_name(&c, 0, b, sizeof b); CHECK(!strcmp(b, "F"), "Minor Pop in A slot 2: %s", b);
    set_chord(0, 5, 7, &c); chord_name(&c, 1, b, sizeof b); CHECK(!strcmp(b, "Db7"), "ii-V-I tritone sub: %s", b);
    CHECK(!parse_chord(0, "x", 1, &c) && !parse_chord(0, "I:nope", 6, &c) && !parse_chord(0, "ivv", 3, &c), "bad tokens parse");
    CHECK(parse_chord(0, "#iv:m7b5", 8, &c) && c.root == 6, "#iv root");
    CHECK(parse_chord(0, "bvii", 4, &c) && c.root == 10 && !strcmp(c.quality, "m"), "bvii minor");

    /* voicings */
    voice_opts_t o = {VOICE_CLOSE, 0, 0, 0, 0};
    voicing_t v, prev = {0};
    diatonic_chord(0, SCALE_MAJOR, 0, EXT_7TH, &c);
    voice_chord(&c, &o, NULL, &v);
    CHECK(v.n == 4 && v.note[0] == 48 && v.note[3] == 59, "Cmaj7 close at C3");
    o.inversion = 1; voice_chord(&c, &o, NULL, &v);
    CHECK(v.note[0] == 52 && v.note[3] == 60, "Cmaj7 1st inversion");
    o.inversion = 0; o.voicing = VOICE_DROP2; voice_chord(&c, &o, NULL, &v);
    CHECK(v.note[0] == 43 && v.n == 4, "Cmaj7 drop 2 (G below)");
    o.voicing = VOICE_CLOSE; o.bass = 1; voice_chord(&c, &o, NULL, &v);
    CHECK(v.n == 5 && v.note[0] == 36, "bass note C2 under the chord");

    /* voice leading: walking around the circle of fifths stays in register and moves little */
    o = (voice_opts_t){VOICE_CLOSE, 0, 0, 0, 1};
    int maxmove = 0;
    for (int i = 0; i < 48; i++) {
        diatonic_chord((i * 7) % 12, SCALE_MAJOR, 0, EXT_7TH, &c);
        voice_chord(&c, &o, &prev, &v);
        for (int k = 0; k < v.n; k++) CHECK(v.note[k] >= 40 && v.note[k] <= 84, "voice leading drifted: %d", v.note[k]);
        if (prev.n) { int m = abs(v.note[0] - prev.note[0]); if (m > maxmove) maxmove = m; }
        prev = v;
    }
    CHECK(maxmove <= 7, "voice leading moved the bottom voice %d semitones", maxmove);
}


static void expect_button(int pc, int type, int ext, const char *want) {
    chord_t c;
    char q[32], got[48];
    int fl;
    button_chord(0, SCALE_MAJOR, pc, type, ext, &c, q, sizeof q, &fl);
    chord_name(&c, fl, got, sizeof got);
    CHECK(!strcmp(got, want), "button %d/%d/%d: got '%s' want '%s'", pc, type, ext, got, want);
}

/* chord-type buttons + extensions, in C major */
static void test_buttons(void) {
    expect_button(2, BT_AUTO, 0, "Dm");                       /* no buttons: the key's triad */
    expect_button(2, BT_AUTO, BX_M7, "Dm7");
    expect_button(2, BT_AUTO, BX_9, "Dm9");                   /* a 9 alone brings the key's 7th */
    expect_button(0, BT_AUTO, BX_9, "Cmaj9");
    expect_button(7, BT_AUTO, BX_9, "G9");
    expect_button(11, BT_AUTO, BX_M7, "Bm7b5");
    expect_button(10, BT_AUTO, BX_M7, "Bb7");                 /* off the scale: the borrowed chord */
    expect_button(0, BT_MAJ, BX_9, "Cmaj9");
    expect_button(9, BT_MIN, BX_6, "Am6");
    expect_button(7, BT_SUS, BX_M7, "G7sus4");
    expect_button(0, BT_SUS, BX_9, "C9sus4");
    expect_button(11, BT_DIM, BX_M7, "Bm7b5");
    expect_button(0, BT_DIM, BX_MAJ7, "CdimMaj7");
    expect_button(0, BT_MIN, BX_MAJ7, "CmMaj7");
    expect_button(0, BT_MAJ, BX_6 | BX_9, "C6/9");
    expect_button(0, BT_MAJ, BX_6 | BX_M7, "C7/6");
    expect_button(2, BT_AUTO, BX_6 | BX_MAJ7, "Dm(6,maj7)");  /* nothing in the table: spelled out */
    /* every combination gives a chord with the triad underneath and no repeated tone */
    for (int pc = 0; pc < 12; pc++)
        for (int t = 0; t < NUM_BTYPES; t++)
            for (int x = 0; x < 16; x++) {
                chord_t c;
                char q[32];
                button_chord(0, SCALE_MAJOR, pc, t, x, &c, q, sizeof q, NULL);
                int ok = c.n >= 3 && c.quality && c.iv[0] == 0;
                for (int i = 1; i < c.n; i++) ok &= c.iv[i] > c.iv[i - 1];
                CHECK(ok, "button %d/%d/%d is a chord", pc, t, x);
            }
}

/* ---- engine, through the capture sink ---- */
static int ev_n, ev[256][3];
static void sink(int ch, int note, int vel) { if (ev_n < 256) { ev[ev_n][0] = ch; ev[ev_n][1] = note; ev[ev_n][2] = vel; ev_n++; } }
static int16_t blk[256];
static void run(const mpc_engine_t *e, void *s, int blocks) { while (blocks--) e->render(s, blk, 128); }
static void send(const mpc_engine_t *e, void *s, int st, int n, int v) { uint8_t m[3] = {(uint8_t)st, (uint8_t)n, (uint8_t)v}; e->midi(s, m, 3); }
static int find_on(int note) { for (int i = 0; i < ev_n; i++) if (ev[i][1] == note && ev[i][2]) return i; return -1; }
static int ons(void) { int n = 0; for (int i = 0; i < ev_n; i++) n += ev[i][2] > 0; return n; }
static int offs(void) { int n = 0; for (int i = 0; i < ev_n; i++) n += ev[i][2] == 0; return n; }

static void test_engine(void) {
    const mpc_engine_t *e = mpc_engine();
    char buf[256];
    seq_out_set_sink(sink);
    void *s = e->create(NULL), *s2 = e->create(NULL);
    CHECK(s && s2, "create");
    e->get_param(s, "status", buf, sizeof buf); CHECK(!strcmp(buf, "MIDI OUT: TEST"), "status %s", buf);
    e->get_param(s, "slot_1", buf, sizeof buf); CHECK(!strcmp(buf, "1  Cmaj7"), "slot_1 (pad number, name) %s", buf);
    e->get_param(s, "slot_8", buf, sizeof buf); CHECK(!strcmp(buf, "8  Cmaj7"), "slot_8 (tonic up) %s", buf);

    /* pad 1 (note 36): Cmaj7 on MIDI channel 2 (the default: off the keys' channel 1, so MPC never merges its echo with a held key), held until note off */
    e->set_param(s, "voice_lead", "0");
    send(e, s, 0x90, 36, 90); run(e, s, 1);
    CHECK(ons() == 4 && ev[0][1] == 48 && ev[0][2] == 90 && ev[0][0] == 1, "pad 1 -> Cmaj7 (%d on)", ons());
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Cmaj7  C3 E3 G3 B3", 18), "chord text '%s'", buf);
    e->get_param(s, "slot_1_on", buf, sizeof buf); CHECK(!strcmp(buf, "1"), "slot_1_on while held");
    send(e, s, 0x80, 36, 0); run(e, s, 1);
    CHECK(offs() == 4, "note off releases all 4 (%d)", offs());
    e->get_param(s, "slot_1_on", buf, sizeof buf); CHECK(!strcmp(buf, "0"), "slot_1_on after release");

    /* pad 13 = slot 5 an octave up, on channel 3 at fixed velocity */
    ev_n = 0;
    e->set_param(s, "channel", "3"); e->set_param(s, "vel_mode", "1"); e->set_param(s, "velocity", "64");
    send(e, s, 0x90, 36 + 12, 127); run(e, s, 1);
    CHECK(ons() == 4 && ev[0][0] == 2 && ev[0][1] == 55 + 12 && ev[0][2] == 64, "pad 13 -> G7 +1 oct ch3 vel64 (%d %d %d)", ev[0][0], ev[0][1], ev[0][2]);
    send(e, s, 0x90, 36 + 12, 0); run(e, s, 1);   /* note on with velocity 0 = off */
    CHECK(offs() == 4, "vel-0 note on releases");

    /* strum: 10 ms between notes -> the 4th note lands ~30 ms (10+ blocks) later */
    ev_n = 0;
    e->set_param(s, "strum", "10"); e->set_param(s, "channel", "1");
    send(e, s, 0x90, 37, 100); run(e, s, 1);
    CHECK(ons() == 1, "strum: first note only in block 1 (%d)", ons());
    run(e, s, 11);
    CHECK(ons() == 4, "strum: all notes after 30 ms (%d)", ons());
    send(e, s, 0x80, 37, 0); run(e, s, 1);
    CHECK(offs() == 4, "strum release");
    /* releasing mid-strum cancels the notes not yet sent */
    ev_n = 0;
    e->set_param(s, "strum", "50");
    send(e, s, 0x90, 37, 100); run(e, s, 1); send(e, s, 0x80, 37, 0); run(e, s, 40);
    CHECK(ons() == 1 && offs() == 1, "mid-strum release (%d on %d off)", ons(), offs());
    e->set_param(s, "strum", "0");

    /* two held chords sharing notes: C (C3 E3 G3) and Em at -1 oct... use triads C and Am, both at C3:
     * Am = A2 C3 E3 via the octave setting, so C3 and E3 are shared. Releasing C must not cut them. */
    ev_n = 0;
    e->set_param(s, "extension", "0");
    send(e, s, 0x90, 36, 100); run(e, s, 1);                 /* C: 48 52 55 */
    e->set_param(s, "octave", "1");
    send(e, s, 0x90, 41, 100); run(e, s, 1);                 /* Am one octave down: 45 48 52 */
    int before = ev_n;
    send(e, s, 0x80, 36, 0); run(e, s, 1);
    int cut = 0, others = 0;
    for (int i = before; i < ev_n; i++) if (ev[i][2] == 0) { if (ev[i][1] == 48 || ev[i][1] == 52) cut++; else others++; }
    CHECK(cut == 0 && others == 1, "releasing C kept the shared notes (cut %d, released %d)", cut, others);
    send(e, s, 0x80, 41, 0); run(e, s, 1);
    e->set_param(s, "extension", "1"); e->set_param(s, "octave", "2");

    /* keys input, White Keys: D4 -> Dm7 at D3; the black key above it (D#4) -> Dm9, the same chord with more colour */
    ev_n = 0;
    e->set_param(s, "input", "1");
    send(e, s, 0x90, 62, 100); run(e, s, 1);
    CHECK(ons() == 4 && ev[0][1] == 50, "keys: D4 -> Dm7 at D3 (%d notes, first %d)", ons(), ev[0][1]);
    e->get_param(s, "slot_2_on", buf, sizeof buf); CHECK(!strcmp(buf, "1"), "keys: D lights slot 2");
    send(e, s, 0x80, 62, 0); run(e, s, 1);
    ev_n = 0;
    send(e, s, 0x90, 63, 100); run(e, s, 1);
    e->get_param(s, "chord", buf, sizeof buf);
    CHECK(ons() == 5 && !strncmp(buf, "Dm9 ", 4), "keys: D#4 -> Dm9 (%d notes, '%s')", ons(), buf);
    send(e, s, 0x80, 63, 0); run(e, s, 1);
    CHECK(offs() == 5, "keys: black key releases");

    /* Scale Notes: every key plays its own chord; Eb (off C major) -> borrowed Ebmaj7 */
    e->set_param(s, "key_map", "1");
    ev_n = 0;
    send(e, s, 0x90, 64, 100); run(e, s, 1);
    e->get_param(s, "chord", buf, sizeof buf);
    CHECK(ons() == 4 && ev[0][1] == 52 && !strncmp(buf, "Em7 ", 4), "scale notes: E4 -> Em7 at E3 ('%s')", buf);
    e->get_param(s, "slot_3_on", buf, sizeof buf); CHECK(!strcmp(buf, "1"), "scale notes: E lights slot 3");
    send(e, s, 0x80, 64, 0);
    send(e, s, 0x90, 51, 100); run(e, s, 1);
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Ebmaj7 ", 7), "scale notes: Eb3 -> Ebmaj7 ('%s')", buf);
    send(e, s, 0x80, 51, 0); run(e, s, 1);

    /* split at C4: below plays chords, from C4 up passes single notes through */
    e->set_param(s, "split", "1");
    e->get_param(s, "split_name", buf, sizeof buf); CHECK(!strcmp(buf, "C4"), "split_name %s", buf);
    ev_n = 0;
    send(e, s, 0x90, 72, 90); run(e, s, 1);
    CHECK(ons() == 1 && ev[0][1] == 72 && ev[0][2] == 64, "split: C5 passes through at the fixed velocity (%d on)", ons());
    send(e, s, 0x90, 55, 90); run(e, s, 1);
    CHECK(ons() == 5, "split: G3 plays a chord (%d on)", ons());
    send(e, s, 0x80, 72, 0); send(e, s, 0x80, 55, 0); run(e, s, 1);
    CHECK(offs() == 5, "split: both release (%d)", offs());
    e->set_param(s, "split", "0"); e->set_param(s, "key_map", "0");
    e->set_param(s, "input", "0");

    /* preview synth: SOUND on (default) makes audio while a chord is held, silence after release; off = silent */
    {
        int16_t out[256];
        long peak = 0;
        send(e, s, 0x90, 36, 100); run(e, s, 1);
        for (int b = 0; b < 20; b++) { e->render(s, out, 128); for (int i = 0; i < 256; i++) if (labs(out[i]) > peak) peak = labs(out[i]); }
        CHECK(peak > 1000 && peak < 32767, "synth: held chord is audible and unclipped (peak %ld)", peak);
        send(e, s, 0x80, 36, 0);
        for (int b = 0; b < 200; b++) e->render(s, out, 128);
        peak = 0;
        e->render(s, out, 128); for (int i = 0; i < 256; i++) if (labs(out[i]) > peak) peak = labs(out[i]);
        CHECK(peak == 0, "synth: silent after the release (peak %ld)", peak);
        e->set_param(s, "sound", "0");
        send(e, s, 0x90, 36, 100); run(e, s, 1);
        peak = 0;
        e->render(s, out, 128); for (int i = 0; i < 256; i++) if (labs(out[i]) > peak) peak = labs(out[i]);
        CHECK(peak == 0, "synth: SOUND off is silent (peak %ld)", peak);
        send(e, s, 0x80, 36, 0); run(e, s, 1);
        e->set_param(s, "sound", "1");
    }

    /* feedback: MPC routes our own port back into this track, so each chord note returns as a key. Echoes
     * are swallowed (no new chords), their note-offs too, and the real key still releases everything. */
    e->set_param(s, "input", "1");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 1);
    int sent = ev_n, echoed[8], ne = 0;
    for (int i = 0; i < sent; i++) echoed[ne++] = ev[i][1];
    for (int i = 0; i < ne; i++) send(e, s, 0x90, echoed[i], 100);
    run(e, s, 1);
    CHECK(ev_n == sent, "feedback: echoed notes play nothing (%d events, want %d)", ev_n, sent);
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    CHECK(offs() == sent, "feedback: the key releases the chord (%d off, want %d)", offs(), sent);
    for (int i = 0; i < ne; i++) send(e, s, 0x80, echoed[i], 0);
    run(e, s, 1);
    int before_key = ev_n;
    send(e, s, 0x90, echoed[0], 100); run(e, s, 1);   /* that note as a real key again: plays */
    CHECK(ev_n > before_key, "feedback: echo note-offs consumed, the note works as a key again");
    send(e, s, 0x80, echoed[0], 0); run(e, s, 1);
    e->set_param(s, "input", "0");

    /* screen tap: ignored while the host restores values, then plays and auto-releases */
    void *s3 = e->create(NULL);
    ev_n = 0;
    e->set_param(s3, "slot_5", "1"); run(e, s3, 2);
    CHECK(ev_n == 0, "tap during load ignored");
    run(e, s3, 200);
    e->set_param(s3, "slot_5", "1"); run(e, s3, 1);
    CHECK(ons() == 4, "tap plays G7");
    run(e, s3, 220);
    CHECK(offs() == 4, "tap auto-releases (%d)", offs());

    /* sets */
    e->set_param(s, "source", "1"); e->set_param(s, "set", "10"); e->set_param(s, "key", "9");
    e->get_param(s, "set_name", buf, sizeof buf); CHECK(!strcmp(buf, "Minor Pop"), "set_name %s", buf);
    e->get_param(s, "slot_2", buf, sizeof buf); CHECK(!strcmp(buf, "2  F"), "Minor Pop slot 2 %s", buf);
    e->set_param(s, "set", "99"); e->get_param(s, "set", buf, sizeof buf); CHECK(atoi(buf) == NUM_CHORD_SETS - 1, "set clamps");

    /* state round trip */
    e->set_param(s, "set", "3");
    char st[1024];
    e->set_param(s, "edit", "4"); e->set_param(s, "edit_root", "7"); e->set_param(s, "edit_qual", "12");   /* pad 5 = Gm7 */
    CHECK(e->get_param(s, "state", st, sizeof st) > 0 && strlen(st) < 900, "state (%zu chars)", strlen(st));
    e->set_param(s2, "state", st);
    e->get_param(s2, "set_name", buf, sizeof buf); CHECK(!strcmp(buf, "Canon"), "state restores set (%s)", buf);
    e->get_param(s2, "key", buf, sizeof buf); CHECK(!strcmp(buf, "9"), "state restores key");
    e->get_param(s2, "cust_root_5", buf, sizeof buf); CHECK(!strcmp(buf, "7"), "state restores a custom root (%s)", buf);
    e->get_param(s2, "cust_qual_5", buf, sizeof buf); CHECK(!strcmp(buf, "12"), "state restores a custom quality (%s)", buf);

    /* CUSTOM chords: a chord builder per pad. Fresh, they are the C major 7ths; a tile tap picks the pad to
     * edit; ROOT and CHORD step it; COPY takes the chords SCALE or SET last showed. */
    CHECK(quality_count() == 43, "43 chord qualities (params.json edit_qual max 42): %d", quality_count());
    void *s4 = e->create(NULL);
    e->set_param(s4, "source", "2");
    e->get_param(s4, "slot_1", buf, sizeof buf); CHECK(!strcmp(buf, "1  Cmaj7   < EDIT"), "custom starts as C major 7ths, pad 1 editing (%s)", buf);
    e->get_param(s4, "slot_7", buf, sizeof buf); CHECK(!strcmp(buf, "7  Bm7b5"), "custom slot 7 (%s)", buf);
    e->get_param(s4, "slot_8", buf, sizeof buf); CHECK(!strcmp(buf, "8  Cmaj7"), "custom slot 8 (the tonic up an octave matched) (%s)", buf);
    e->get_param(s4, "edit_name", buf, sizeof buf); CHECK(!strcmp(buf, "PAD 1"), "edit_name (%s)", buf);
    run(e, s4, 200);                                      /* past the quiet time after create */
    e->set_param(s4, "slot_3", "1"); run(e, s4, 1);       /* a tile tap plays it and picks it to edit */
    e->get_param(s4, "edit", buf, sizeof buf); CHECK(!strcmp(buf, "2"), "tile tap picks the edit slot (%s)", buf);
    e->get_param(s4, "edit_root_name", buf, sizeof buf); CHECK(!strcmp(buf, "E"), "edit root (%s)", buf);
    e->get_param(s4, "edit_qual_name", buf, sizeof buf); CHECK(!strcmp(buf, "m7"), "edit quality (%s)", buf);
    e->set_param(s4, "edit_root", "3"); e->set_param(s4, "edit_qual", "0");
    e->get_param(s4, "slot_3", buf, sizeof buf); CHECK(!strcmp(buf, "3  D#   < EDIT"), "edited to D# major, spelt as the key does (%s)", buf);
    e->get_param(s4, "edit_qual_name", buf, sizeof buf); CHECK(!strcmp(buf, "maj"), "a major triad reads maj (%s)", buf);
    e->set_param(s4, "edit_qual", "99");
    e->get_param(s4, "edit_qual", buf, sizeof buf); CHECK(atoi(buf) == quality_count() - 1, "quality clamps (%s)", buf);
    e->set_param(s4, "edit_qual", "0");
    run(e, s4, 250);                                      /* the tap's chord has ended */
    ev_n = 0;
    send(e, s4, 0x90, 38, 100); run(e, s4, 1);            /* pad 3 */
    int pcs = 0;
    for (int i = 0; i < ev_n; i++) if (ev[i][2]) pcs |= 1 << (ev[i][1] % 12);
    CHECK(ons() == 3 && pcs == ((1 << 3) | (1 << 7) | (1 << 10)), "pad 3 plays the custom D# major (%d on, pcs %x)", ons(), pcs);
    send(e, s4, 0x80, 38, 0); run(e, s4, 1);
    e->set_param(s4, "input", "1");                       /* on the white keys: E plays pad 3's chord */
    e->get_param(s4, "slot_3", buf, sizeof buf); CHECK(!strncmp(buf, "E  ", 3), "keys: tiles show the key letter (%s)", buf);
    ev_n = 0;
    send(e, s4, 0x90, 64, 100); run(e, s4, 1);
    CHECK(ons() == 3 && find_on(63) >= 0, "white key E plays custom chord 3 (%d on)", ons());
    send(e, s4, 0x80, 64, 0); run(e, s4, 1);
    e->set_param(s4, "input", "0");
    /* COPY: from the set that showed last */
    e->set_param(s4, "source", "1"); e->set_param(s4, "set", "0");   /* Pop Axis: I V vi IV ... */
    e->set_param(s4, "source", "2"); e->set_param(s4, "copy", "1");
    e->get_param(s4, "slot_1", buf, sizeof buf); CHECK(!strcmp(buf, "1  C"), "copied Pop Axis pad 1 (%s)", buf);
    e->get_param(s4, "slot_3", buf, sizeof buf); CHECK(!strcmp(buf, "3  Am   < EDIT"), "copied Pop Axis pad 3 (%s)", buf);
    e->get_param(s4, "chord", buf, sizeof buf); CHECK(!strcmp(buf, "COPIED: Pop Axis"), "copy says what it took (%s)", buf);
    e->set_param(s4, "source", "0"); e->set_param(s4, "scale", "1"); e->set_param(s4, "key", "9");   /* A minor */
    e->set_param(s4, "source", "2"); e->set_param(s4, "copy", "1");
    e->get_param(s4, "slot_1", buf, sizeof buf); CHECK(!strcmp(buf, "1  Am7"), "copied A minor pad 1 (%s)", buf);
    e->get_param(s4, "chord", buf, sizeof buf); CHECK(!strcmp(buf, "COPIED: A Minor"), "copy names the scale (%s)", buf);
    /* the PLAYED trail: oldest first, repeats folded, the newest kept within 47 characters */
    e->get_param(s4, "trail", buf, sizeof buf); CHECK(strstr(buf, "PLAYED: ") == buf, "trail starts with PLAYED: (%s)", buf);
    e->set_param(s4, "source", "0"); e->set_param(s4, "scale", "0"); e->set_param(s4, "key", "0");
    void *s5 = e->create(NULL);
    e->get_param(s5, "trail", buf, sizeof buf); CHECK(!strcmp(buf, "PLAYED: -"), "empty trail (%s)", buf);
    for (int i = 0; i < 3; i++) { send(e, s5, 0x90, 36 + i, 100); run(e, s5, 1); send(e, s5, 0x80, 36 + i, 0); run(e, s5, 1); }
    send(e, s5, 0x90, 38, 100); run(e, s5, 1); send(e, s5, 0x80, 38, 0); run(e, s5, 1);   /* Em7 again: folded */
    e->get_param(s5, "trail", buf, sizeof buf); CHECK(!strcmp(buf, "PLAYED: Cmaj7 > Dm7 > Em7"), "trail (%s)", buf);
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 8; i++) { send(e, s5, 0x90, 36 + i, 100); run(e, s5, 1); send(e, s5, 0x80, 36 + i, 0); run(e, s5, 1); }
    e->get_param(s5, "trail", buf, sizeof buf);
    CHECK(strlen(buf) <= 47 && strstr(buf, "> Cmaj7") && buf[strlen(buf) - 1] == '7', "trail stays within the readout, newest last (%zu: %s)", strlen(buf), buf);
    e->destroy(s5);
    e->destroy(s4);

    /* panic */
    ev_n = 0;
    send(e, s, 0x90, 36, 100); run(e, s, 1);
    e->set_param(s, "panic", "1"); run(e, s, 1);
    CHECK(offs() == ons(), "panic releases everything (%d/%d)", offs(), ons());


    e->destroy(s); e->destroy(s2); e->destroy(s3);
}


/* INPUT = BUTTONS, LATCH, the VOICING dial, perform modes against the host tempo, the bass channel, echo clash */
static void test_perform(void) {
    const mpc_engine_t *e = mpc_engine();
    char buf[256];
    void *s = e->create(NULL);
    e->set_param(s, "voice_lead", "0");
    e->get_param(s, "buttons_hint", buf, sizeof buf); CHECK(!strcmp(buf, "SET PLAY FROM TO BUTTONS TO USE THESE"), "hint off '%s'", buf);
    e->set_param(s, "input", "2");
    e->get_param(s, "input", buf, sizeof buf); CHECK(!strcmp(buf, "2"), "input 2 = buttons (%s)", buf);
    e->get_param(s, "buttons_hint", buf, sizeof buf); CHECK(!strncmp(buf, "PADS 1-8", 8) && strlen(buf) < 48, "hint on '%s'", buf);
    e->get_param(s, "output", buf, sizeof buf); CHECK(!strcmp(buf, "PREVIEW SYNTH + MIDI OUT CH 2"), "output '%s'", buf);
    e->set_param(s, "sound", "0");
    e->get_param(s, "output", buf, sizeof buf); CHECK(!strcmp(buf, "MIDI OUT CH 2 ONLY (PREVIEW SYNTH OFF)"), "output, synth off '%s'", buf);
    e->set_param(s, "sound", "1");
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strcmp(buf, "PLAY A PAD, OR TAP A CHORD"), "idle text '%s'", buf);

    /* pad 10 (note 45): root ii = Dm, out on channel 2 */
    ev_n = 0;
    send(e, s, 0x90, 45, 100); run(e, s, 1);
    CHECK(ons() == 3 && ev[0][0] == 1 && ev[0][1] == 50, "root pad ii -> Dm (%d on, %d)", ons(), ev[0][1]);
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Dm  ", 4), "chord '%s'", buf);
    /* hold m7 (pad 2): re-chords the held root */
    ev_n = 0;
    send(e, s, 0x90, 37, 100); run(e, s, 1);
    CHECK(offs() == 3 && ons() == 4, "m7 button re-chords (%d off, %d on)", offs(), ons());
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Dm7 ", 4), "chord '%s'", buf);
    /* and dim (pad 5) on top: Dm7b5; let go of dim and it is back to the key's chord */
    send(e, s, 0x90, 40, 100); run(e, s, 1);
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Dm7b5 ", 6), "dim + m7 '%s'", buf);
    send(e, s, 0x90, 42, 100); run(e, s, 1);   /* maj while dim held: the last type pressed wins */
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "D7 ", 3), "maj + m7 '%s'", buf);
    send(e, s, 0x80, 42, 0); run(e, s, 1);
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Dm7b5 ", 6), "back to dim '%s'", buf);
    send(e, s, 0x80, 40, 0); send(e, s, 0x80, 37, 0); run(e, s, 1);
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Dm  ", 4), "buttons up -> Dm '%s'", buf);
    ev_n = 0;
    send(e, s, 0x80, 45, 0); run(e, s, 1);
    CHECK(offs() == 3 && ons() == 0, "root up releases (%d)", offs());
    /* feedback during a re-chord: MPC echoes every note-on and note-off we send. The re-chord sends D F A off
     * and D F A C on together; the echoed offs must not get through as keys let go (they did, and in BUTTONS
     * mode every leaked note played another chord: a runaway cascade on the device) */
    {
        int echo_ev[64][3], m = 0, from = 0;
        ev_n = 0;
        send(e, s, 0x90, 62, 100); run(e, s, 1);
        send(e, s, 0x90, 37, 100); run(e, s, 1);   /* m7 while D is held */
        for (int round = 0; round < 3; round++) {   /* echo back what came out, and any echo of that */
            int to = ev_n;
            for (int i = from; i < to && m < 64; i++, m++) {
                memcpy(echo_ev[m], ev[i], sizeof ev[i]);
                send(e, s, (ev[i][2] ? 0x90 : 0x80) | ev[i][0], ev[i][1], ev[i][2]);
            }
            run(e, s, 1);
            from = to;
        }
        CHECK(ev_n == 3 + 3 + 4, "re-chord echoes play nothing (%d events, want 10)", ev_n);
        e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Dm7 ", 4), "still Dm7 after the echoes '%s'", buf);
        send(e, s, 0x80, 37, 0); run(e, s, 1);
        send(e, s, 0x80, 62, 0); run(e, s, 1);
        int bal[128] = {0}, bad = 0;
        for (int i = 0; i < ev_n; i++) bal[ev[i][1]] += ev[i][2] ? 1 : -1;
        for (int n = 0; n < 128; n++) bad += bal[n] != 0;
        CHECK(!bad, "every note released after the echoed re-chord (%d hanging)", bad);
        for (int i = from; i < ev_n; i++) send(e, s, (ev[i][2] ? 0x90 : 0x80) | ev[i][0], ev[i][1], ev[i][2]);
        run(e, s, 1);
        (void)echo_ev;
    }
    /* a fast arp: a note is off again before its echo is back; the late echoes still play nothing */
    e->set_param(s, "perform", "2"); e->set_param(s, "rate", "5");
    ev_n = 0;
    send(e, s, 0x90, 60, 100);
    for (int k = 0, from = 0; k < 120; k++) {   /* echo everything 4 blocks (12 ms) late */
        run(e, s, 1);
        int to = k >= 4 ? ev_n : 0;
        for (int i = from; i < to; i++) send(e, s, (ev[i][2] ? 0x90 : 0x80) | ev[i][0], ev[i][1], ev[i][2]);
        if (to) from = to;
    }
    {
        int distinct = 0, seen[128] = {0};
        for (int i = 0; i < ev_n; i++) if (ev[i][2] && !seen[ev[i][1]]++) distinct++;
        CHECK(distinct == 3, "arp echoes don't start chords (%d distinct notes)", distinct);
    }
    send(e, s, 0x80, 60, 0); run(e, s, 4);
    e->set_param(s, "perform", "0"); e->set_param(s, "rate", "3");
    /* a key outside the pads plays its own root; screen type + extension apply */
    e->set_param(s, "chord_type", "3"); e->set_param(s, "ext_9", "1");
    ev_n = 0;
    send(e, s, 0x90, 62, 100); run(e, s, 1);
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Dmaj9 ", 6) && ons() == 5, "key D, MAJ + 9 '%s'", buf);
    send(e, s, 0x80, 62, 0); run(e, s, 1);
    e->set_param(s, "chord_type", "0"); e->set_param(s, "ext_9", "0");
    /* PAD BUTTONS off: pads are roots like keys */
    e->set_param(s, "pad_buttons", "0");
    ev_n = 0;
    send(e, s, 0x90, 37, 100); run(e, s, 1);
    e->get_param(s, "chord", buf, sizeof buf); CHECK(!strncmp(buf, "Db ", 3) && ons() == 3, "pad 2 as a root '%s'", buf);
    send(e, s, 0x80, 37, 0); run(e, s, 1);
    e->set_param(s, "pad_buttons", "1");

    /* LATCH: a chord sounds after the key is let go, until the next chord or LATCH off */
    e->set_param(s, "latch", "1");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 1); send(e, s, 0x80, 60, 0); run(e, s, 10);
    CHECK(ons() == 3 && offs() == 0, "latched C holds (%d off)", offs());
    send(e, s, 0x90, 37, 100); run(e, s, 1);
    CHECK(offs() == 3 && ons() == 7, "a button re-chords the latched chord (%d off, %d on)", offs(), ons());
    send(e, s, 0x80, 37, 0); run(e, s, 1);
    ev_n = 0;
    send(e, s, 0x90, 67, 100); run(e, s, 1);
    CHECK(offs() == 3 && ons() == 3 && ev[3][1] == 55, "the next chord ends it (%d off, %d on)", offs(), ons());
    send(e, s, 0x80, 67, 0); run(e, s, 1);
    ev_n = 0;
    e->set_param(s, "latch", "0"); run(e, s, 1);
    CHECK(offs() == 3, "LATCH off releases (%d)", offs());

    /* VOICING dial: +1 moves the bottom note up an octave (C E G -> E G C), -1 the top one down */
    e->set_param(s, "voice_dial", "1");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 1);
    CHECK(ons() == 3 && ev[0][1] == 52 && find_on(60) >= 0, "dial +1: E G C (%d)", ev[0][1]);
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    e->set_param(s, "voice_dial", "-1");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 1);
    CHECK(ons() == 3 && ev[0][1] == 43 && find_on(55) < 0, "dial -1: G C E (%d)", ev[0][1]);
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    e->set_param(s, "voice_dial", "0");

    /* BASS on its own channel */
    e->set_param(s, "bass", "1"); e->set_param(s, "bass_ch", "5");
    e->get_param(s, "bass_ch_name", buf, sizeof buf); CHECK(!strcmp(buf, "5"), "bass_ch_name '%s'", buf);
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 1);
    CHECK(ons() == 4 && ev[0][0] == 4 && ev[0][1] == 36 && ev[1][0] == 1, "bass C2 on ch5, chord on ch2 (%d %d)", ev[0][0], ev[1][0]);
    ev_n = 0;
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    CHECK(offs() == 4 && ev[0][0] == 4, "bass note off on ch5");
    e->set_param(s, "bass", "0"); e->set_param(s, "bass_ch", "0");
    e->get_param(s, "bass_ch_name", buf, sizeof buf); CHECK(!strcmp(buf, "SAME"), "bass_ch_name '%s'", buf);

    /* ARP UP at 1/16, 120 BPM: a note every 5512 frames, one at a time, round the chord */
    e->set_param(s, "perform", "2");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 200);   /* 25600 frames: steps at 0, 5512, ... 22050 */
    int seq[8], k = 0;
    for (int i = 0; i < ev_n && k < 8; i++) if (ev[i][2]) seq[k++] = ev[i][1];
    CHECK(k == 5 && seq[0] == 48 && seq[1] == 52 && seq[2] == 55 && seq[3] == 48, "arp up C E G C (%d notes)", k);
    CHECK(offs() == 4, "arp: one note at a time (%d off)", offs());
    ev_n = 0;
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    CHECK(offs() == 1 && ons() == 0, "arp stops on release (%d off)", offs());
    /* the host tempo: 60 BPM doubles the step */
    e->set_param(s, "lfo_bpm", "60.00");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 200);
    CHECK(ons() == 3, "arp at 60 BPM: 3 steps in 25600 frames (%d)", ons());
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    e->set_param(s, "lfo_bpm", "120.00");
    /* ARP DOWN */
    e->set_param(s, "perform", "3");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 100);
    CHECK(ons() >= 2 && ev[0][1] == 55 && find_on(52) > 0, "arp down starts at G");
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    /* PATTERN 2 (every other 1/16): the whole chord on steps 0, 2, 4 */
    e->set_param(s, "perform", "7");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 200);
    CHECK(ons() == 9, "pattern 2: three hits of three notes (%d)", ons());
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    int bal = 0;
    for (int i = 0; i < ev_n; i++) bal += ev[i][2] ? 1 : -1;
    CHECK(bal == 0, "pattern: every note-on has its note-off (%d)", bal);
    /* HARP: the chord over four octaves, a 1/64 apart */
    e->set_param(s, "perform", "5");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 1);
    CHECK(ons() == 1, "harp starts with one note (%d)", ons());
    run(e, s, 130);
    CHECK(ons() == 12 && find_on(55 + 36) > 0, "harp: 12 notes up four octaves (%d)", ons());
    ev_n = 0;
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    CHECK(offs() == 12, "harp releases all (%d)", offs());
    /* SLOP: all notes inside 40 ms */
    e->set_param(s, "perform", "1");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 14);
    CHECK(ons() == 3, "slop: all three within 40 ms (%d)", ons());
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    e->set_param(s, "perform", "0");

    /* echo clash: output on the keys' channel and a chord holding the key's own note */
    e->set_param(s, "input", "1"); e->set_param(s, "channel", "1"); e->set_param(s, "octave", "3");
    send(e, s, 0x90, 60, 100); run(e, s, 1);   /* Cmaj7 C4 E4 G4 B4: C4 is the key */
    send(e, s, 0x90, 60, 100); run(e, s, 1);   /* MPC sends it straight back */
    e->get_param(s, "status", buf, sizeof buf); CHECK(strstr(buf, "SAME CH") != NULL && strlen(buf) < 24, "echo clash warned '%s'", buf);
    e->get_param(s, "output", buf, sizeof buf); CHECK(strstr(buf, "KEYS' CHANNEL") != NULL && strlen(buf) < 48, "output warns too '%s'", buf);
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    e->set_param(s, "channel", "2");
    e->get_param(s, "status", buf, sizeof buf); CHECK(!strcmp(buf, "MIDI OUT: TEST"), "another channel clears it '%s'", buf);
    /* what the device really does: the merged note's echo never comes back at all, only the other notes' do
     * (and the key's note-off is lost). The missing echo is the tell, once the others are in. */
    e->set_param(s, "channel", "1");
    ev_n = 0;
    send(e, s, 0x90, 60, 100); run(e, s, 1);
    CHECK(ons() == 4 && find_on(60) >= 0, "clash chord holds the key's note (%d on)", ons());
    for (int i = 0; i < ev_n; i++) if (ev[i][1] != 60) send(e, s, 0x90 | ev[i][0], ev[i][1], ev[i][2]);
    run(e, s, 2);
    e->get_param(s, "output", buf, sizeof buf); CHECK(strstr(buf, "KEYS' CHANNEL") == NULL, "no warning before the echo window is up '%s'", buf);
    run(e, s, 100);   /* 290 ms: past ECHO_MS */
    e->get_param(s, "output", buf, sizeof buf); CHECK(strstr(buf, "KEYS' CHANNEL") != NULL, "a missing echo warns '%s'", buf);
    e->set_param(s, "channel", "2"); e->set_param(s, "octave", "2"); e->set_param(s, "input", "0");
    send(e, s, 0x80, 60, 0); run(e, s, 1);
    /* no echoes at all (port not enabled as an input): nothing to warn about */
    void *s2 = e->create(NULL);
    e->set_param(s2, "input", "1"); e->set_param(s2, "channel", "1"); e->set_param(s2, "octave", "3");
    send(e, s2, 0x90, 60, 100); run(e, s2, 120);
    e->get_param(s2, "output", buf, sizeof buf); CHECK(strstr(buf, "KEYS' CHANNEL") == NULL, "no echoes, no warning '%s'", buf);
    send(e, s2, 0x80, 60, 0); run(e, s2, 1);
    e->destroy(s2);
    e->destroy(s);
}

int main(void) {
#ifdef HAVE_ALSA
    CHECK(sizeof(snd_seq_event_t) == 28, "snd_seq_event_t is %zu bytes", sizeof(snd_seq_event_t));
    CHECK(offsetof(snd_seq_event_t, data.note.note) == 17, "note offset");
    CHECK(SND_SEQ_EVENT_NOTEON == 6 && SND_SEQ_ADDRESS_SUBSCRIBERS == 254 && SND_SEQ_QUEUE_DIRECT == 253 &&
          SND_SEQ_PORT_CAP_SUBS_READ == 32 && SND_SEQ_PORT_TYPE_APPLICATION == (1 << 20), "ALSA constants");
#endif
    test_theory();
    test_buttons();
    test_engine();
    test_perform();
    printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
