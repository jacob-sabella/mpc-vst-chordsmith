/* Chord theory: scales, diatonic chords, chord sets, names and voicings. Pure functions, no I/O,
 * so tests/theory_test.c can check them on x86 without a host. */
#pragma once

#define MAX_TONES 7      /* intervals in one chord quality (13th = 6) */
#define MAX_VOICE 8      /* notes in one voiced chord, bass included */
#define NUM_SLOTS 8      /* chords on the pads / screen */

enum { SCALE_MAJOR, SCALE_MINOR, SCALE_HARM_MINOR, SCALE_MEL_MINOR, SCALE_DORIAN, SCALE_PHRYGIAN,
       SCALE_LYDIAN, SCALE_MIXOLYDIAN, SCALE_LOCRIAN, NUM_SCALES };
enum { EXT_TRIAD, EXT_7TH, EXT_9TH, EXT_SUS2, EXT_SUS4, NUM_EXTS };
enum { VOICE_CLOSE, VOICE_DROP2, VOICE_DROP3, VOICE_SPREAD, NUM_VOICINGS };

typedef struct {
    int root;                  /* pitch class 0..11 */
    int n;                     /* tones; 0 = empty slot */
    int iv[MAX_TONES];         /* semitones above the root, ascending, iv[0] == 0 */
    const char *quality;       /* "m7", "maj9", ... ("" for a major triad) */
} chord_t;

typedef struct {
    int n;                     /* notes, ascending */
    int note[MAX_VOICE];       /* MIDI 0..127 */
} voicing_t;

typedef struct { const char *name, *chords; } chord_set_t;

extern const char *const SCALE_NAMES[NUM_SCALES];
extern const chord_set_t CHORD_SETS[];
extern const int NUM_CHORD_SETS;

/* Degree d (0..6) of a scale on key, stacked in thirds (or sus) per ext. */
void diatonic_chord(int key, int scale, int degree, int ext, chord_t *out);
/* Degree (0..6) of pitch class pc in key/scale, or -1 if it isn't in the scale. */
int scale_degree(int key, int scale, int pc);
/* The chord a keyboard key plays in key/scale: the diatonic chord on pc, or for a note outside the scale a
 * borrowed one (the parallel major/minor's chord, the Neapolitan bII, else a dim7). Returns 1 if diatonic.
 * *flats (may be NULL): how to spell its root (a borrowed bIII is Eb in C, a #iv dim7 F#). */
int key_chord(int key, int scale, int pc, int ext, chord_t *out, int *flats);
/* c with more colour, for black keys: a triad gets its 7th, a 7th chord its 9th (both picked from the
 * key/scale where they fit), a 9th chord or bigger goes back to its triad. */
void vary_chord(const chord_t *c, int key, int scale, chord_t *out);
/* Slot i of chord set s transposed to key; returns 0 (n = 0) past the set's last chord. */
int set_chord(int key, int set, int slot, chord_t *out);
/* Parse one set token ("vi", "bVII", "ii:m9", "V:7sus4") against key; 0 on error. */
int parse_chord(int key, const char *tok, int len, chord_t *out);

/* The quality table, for detection and for saving captured chords by index. */
int quality_count(void);
int quality_index(const char *name);              /* -1 if unknown */
int chord_from_quality(int root, int qi, chord_t *out);   /* 0 if qi is out of range */
const char *quality_name(int qi);                 /* "" for a major triad; NULL if out of range */
int quality_of(const chord_t *c);                 /* index of c's tones in the table, else of the longest match from the root; -1 if none */

/* Note name of a pitch class, spelled for the key (flats in flat keys). */
const char *pc_name(int pc, int flats);
/* Whether a key/scale is spelled with flats. */
int key_uses_flats(int key, int scale);
/* "Cmaj7", "F#m7b5", "-" for an empty chord. */
void chord_name(const chord_t *c, int flats, char *buf, int len);

/* Button chords: a triad type (AUTO = the chord the key/scale gives that root) plus extension buttons.
 * A 9 with no 6 or 7 also adds the 7th (the key's for AUTO, maj7 for MAJ, else b7). qbuf (24+ bytes) holds the
 * name when the quality table has no such chord; *flats (may be NULL) how to spell it. Returns 1 when an AUTO
 * chord is diatonic. */
enum { BT_AUTO, BT_DIM, BT_MIN, BT_MAJ, BT_SUS, NUM_BTYPES };
enum { BX_6 = 1, BX_M7 = 2, BX_MAJ7 = 4, BX_9 = 8 };
int button_chord(int key, int scale, int pc, int type, int ext, chord_t *out, char *qbuf, int qlen, int *flats);

typedef struct {
    int voicing;     /* VOICE_* */
    int inversion;   /* 0..3, wraps by chord size */
    int octave;      /* -2..+2 around C3..C4 */
    int bass;        /* add the root an octave below the chord */
    int voice_lead;  /* pick the inversion/octave nearest prev (ignores inversion) */
} voice_opts_t;

/* Voice c; prev (may be NULL or empty) is the last chord played, for voice leading. */
void voice_chord(const chord_t *c, const voice_opts_t *o, const voicing_t *prev, voicing_t *out);
