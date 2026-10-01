#include "theory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const SCALE_NAMES[NUM_SCALES] = {
    "Major", "Minor", "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
};
static const int SCALES[NUM_SCALES][7] = {
    {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 2, 3, 5, 7, 9, 11},
    {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 5, 7, 9, 10},
    {0, 1, 3, 5, 6, 8, 10},
};
/* semitones from the scale's tonic down to the major key that spells it (relative major) */
static const int PARENT_MAJOR[NUM_SCALES] = {0, 3, 3, 3, 10, 8, 7, 5, 1};

typedef struct { const char *name; int n; int iv[MAX_TONES]; } quality_t;
/* Every quality a set may name, and every one the scales produce when stacked in thirds
 * (theory_test.c checks the latter). Order matters only for lookups by interval: first match wins. */
static const quality_t QUALITIES[] = {
    {"", 3, {0, 4, 7}}, {"m", 3, {0, 3, 7}}, {"dim", 3, {0, 3, 6}}, {"aug", 3, {0, 4, 8}},
    {"sus2", 3, {0, 2, 7}}, {"sus4", 3, {0, 5, 7}},
    {"6", 4, {0, 4, 7, 9}}, {"m6", 4, {0, 3, 7, 9}}, {"add9", 4, {0, 4, 7, 14}}, {"madd9", 4, {0, 3, 7, 14}},
    {"maj7", 4, {0, 4, 7, 11}}, {"7", 4, {0, 4, 7, 10}}, {"m7", 4, {0, 3, 7, 10}}, {"m7b5", 4, {0, 3, 6, 10}},
    {"dim7", 4, {0, 3, 6, 9}}, {"mMaj7", 4, {0, 3, 7, 11}}, {"maj7#5", 4, {0, 4, 8, 11}}, {"7sus4", 4, {0, 5, 7, 10}},
    {"6/9", 5, {0, 4, 7, 9, 14}}, {"maj9", 5, {0, 4, 7, 11, 14}}, {"9", 5, {0, 4, 7, 10, 14}},
    {"m9", 5, {0, 3, 7, 10, 14}}, {"m9b5", 5, {0, 3, 6, 10, 14}}, {"7b9", 5, {0, 4, 7, 10, 13}},
    {"7#9", 5, {0, 4, 7, 10, 15}}, {"m7b9", 5, {0, 3, 7, 10, 13}}, {"m7b5b9", 5, {0, 3, 6, 10, 13}},
    {"mMaj9", 5, {0, 3, 7, 11, 14}}, {"maj9#5", 5, {0, 4, 8, 11, 14}}, {"maj7#9", 5, {0, 4, 7, 11, 15}},
    {"dim7b9", 5, {0, 3, 6, 9, 13}}, {"9sus4", 5, {0, 5, 7, 10, 14}}, {"maj7#11", 5, {0, 4, 7, 11, 18}},
    {"m11", 6, {0, 3, 7, 10, 14, 17}}, {"13", 6, {0, 4, 7, 10, 14, 21}},
    /* added for button chords; append only (captured chords are saved by index) */
    {"m6/9", 5, {0, 3, 7, 9, 14}}, {"6sus4", 4, {0, 5, 7, 9}}, {"maj7sus4", 4, {0, 5, 7, 11}},
    {"maj9sus4", 5, {0, 5, 7, 11, 14}}, {"sus4add9", 4, {0, 5, 7, 14}}, {"dimMaj7", 4, {0, 3, 6, 11}},
    {"7/6", 5, {0, 4, 7, 9, 10}}, {"m7/6", 5, {0, 3, 7, 9, 10}},
};
#define NUM_QUALITIES ((int)(sizeof QUALITIES / sizeof QUALITIES[0]))

/* Chords are written as roman numerals over the MAJOR scale of the key, so a minor set reads
 * "i bVI bIII bVII". A bare numeral is a major (upper case) or minor (lower case) triad; ":<quality>"
 * names anything else. Up to NUM_SLOTS chords per set. */
const chord_set_t CHORD_SETS[] = {
    {"Pop Axis", "I V vi IV ii iii I:sus4 V:sus4"},
    {"Sensitive", "vi IV I V ii:m7 IV:maj7 I:add9 V:sus4"},
    {"Fifties", "I vi IV V I:6 vi:m7 ii:m7 V:7"},
    {"Canon", "I V vi iii IV I IV V"},
    {"Royal Road", "IV:maj7 V:7 iii:m7 vi:m7 ii:m7 V:7sus4 I:maj7 III:7"},
    {"Jazz ii-V-I", "ii:m9 V:13 I:maj9 vi:m9 iii:m7 VI:7b9 II:9 bII:7"},
    {"Neo Soul", "IV:maj9 iii:m7 vi:m9 ii:m11 V:9sus4 I:maj9 bVII:9 III:7#9"},
    {"Lo-Fi", "ii:m9 V:9sus4 I:maj7 vi:m7 IV:maj7 iii:m7 bVI:maj7 V:7b9"},
    {"Gospel", "I:add9 IV:6/9 V:9sus4 vi:m11 iii:m7 VI:7 ii:m9 #iv:m7b5"},
    {"R&B Slow Jam", "I:maj9 vi:m11 ii:m9 V:13 iii:m7 VI:7b9 IV:maj7 bVII:9"},
    {"Minor Pop", "i bVI bIII bVII iv v i:m7 bVII:add9"},
    {"Trap Minor", "i bVI:maj7 v i:madd9 iv:m7 bII:maj7 bVII V:7b9"},
    {"Andalusian", "i bVII bVI V V:7b9 iv bII:maj7 i:m7"},
    {"Cinematic", "i bVI bIII bVII iv bVI:maj7 i:sus2 V"},
    {"Dorian Groove", "i:m7 IV:7 i:m9 IV:9 bVII:maj7 v:m7 bIII:maj7 ii:m7"},
    {"House Stabs", "i:m7 iv:m9 bVII:7sus4 bIII:maj9 i:m9 v:m7 bVI:maj7 iv:m11"},
    {"Mixolydian Rock", "I bVII IV I:sus4 bVII:add9 v IV:add9 bVI"},
    {"Blues", "I:7 IV:7 V:7 I:9 IV:9 V:7#9 bVII:7 #iv:dim7"},
    {"Lydian Dream", "I:maj7 II:add9 vii:m7 iii:m7 I:maj9 II:7 vi:m9 V:6/9"},
    {"Phrygian Dark", "i bII bVII i:m7 bII:maj7 iv:m7 bvii bIII"},
};
const int NUM_CHORD_SETS = (int)(sizeof CHORD_SETS / sizeof CHORD_SETS[0]);

static void set_quality(chord_t *c, const quality_t *q) {
    c->quality = q->name;
    c->n = q->n;
    memcpy(c->iv, q->iv, sizeof c->iv);
}

static const quality_t *find_quality(const char *name, int len) {
    for (int i = 0; i < NUM_QUALITIES; i++)
        if ((int)strlen(QUALITIES[i].name) == len && !strncmp(QUALITIES[i].name, name, len)) return &QUALITIES[i];
    return NULL;
}

int quality_count(void) { return NUM_QUALITIES; }

int quality_index(const char *name) {
    const quality_t *q = find_quality(name, (int)strlen(name));
    return q ? (int)(q - QUALITIES) : -1;
}

const char *quality_name(int qi) { return qi < 0 || qi >= NUM_QUALITIES ? NULL : QUALITIES[qi].name; }

int quality_of(const chord_t *c) {
    int iv[MAX_TONES];
    if (!c || c->n < 3 || c->n > MAX_TONES) return -1;
    for (int i = 0; i < c->n; i++) iv[i] = c->iv[i] - c->iv[0];   /* a chord an octave up starts at 12 */
    for (int n = c->n; n >= 3; n--)   /* the whole chord, else without its top notes */
        for (int q = 0; q < NUM_QUALITIES; q++)
            if (QUALITIES[q].n == n && !memcmp(QUALITIES[q].iv, iv, n * sizeof(int))) return q;
    return -1;
}

int chord_from_quality(int root, int qi, chord_t *out) {
    memset(out, 0, sizeof *out);
    if (qi < 0 || qi >= NUM_QUALITIES) return 0;
    out->root = ((root % 12) + 12) % 12;
    set_quality(out, &QUALITIES[qi]);
    return 1;
}

void diatonic_chord(int key, int scale, int degree, int ext, chord_t *out) {
    static const int STACK[NUM_EXTS][5] = {{0, 2, 4}, {0, 2, 4, 6}, {0, 2, 4, 6, 8}, {0, 1, 4}, {0, 3, 4}};
    static const int STACK_N[NUM_EXTS] = {3, 4, 5, 3, 3};
    const int *s = SCALES[scale < 0 || scale >= NUM_SCALES ? 0 : scale];
    if (ext < 0 || ext >= NUM_EXTS) ext = EXT_TRIAD;
    degree = ((degree % 7) + 7) % 7;
    int base = s[degree];
    memset(out, 0, sizeof *out);
    out->root = (key + base) % 12;
    out->n = STACK_N[ext];
    for (int i = 0; i < out->n; i++) {
        int d = degree + STACK[ext][i];
        out->iv[i] = s[d % 7] + 12 * (d / 7) - base;
    }
    out->quality = "?";
    for (int q = 0; q < NUM_QUALITIES; q++)
        if (QUALITIES[q].n == out->n && !memcmp(QUALITIES[q].iv, out->iv, out->n * sizeof(int))) {
            out->quality = QUALITIES[q].name;
            break;
        }
}

int scale_degree(int key, int scale, int pc) {
    const int *s = SCALES[scale < 0 || scale >= NUM_SCALES ? 0 : scale];
    int rel = ((pc - key) % 12 + 12) % 12;
    for (int d = 0; d < 7; d++)
        if (s[d] == rel) return d;
    return -1;
}

static const quality_t *match_quality(int n, const int *iv) {
    for (int q = 0; q < NUM_QUALITIES; q++)
        if (QUALITIES[q].n == n && !memcmp(QUALITIES[q].iv, iv, n * sizeof(int))) return &QUALITIES[q];
    return NULL;
}

int key_chord(int key, int scale, int pc, int ext, chord_t *out, int *flats) {
    int d = scale_degree(key, scale, pc), fl;
    if (!flats) flats = &fl;
    if (d >= 0) {
        diatonic_chord(key, scale, d, ext, out);
        *flats = key_uses_flats(key, scale);
        return 1;
    }
    /* a scale with a minor third borrows from the parallel major, the others from the parallel minor */
    int par = scale_degree(0, scale, 3) >= 0 ? SCALE_MAJOR : SCALE_MINOR;
    if ((d = scale_degree(key, par, pc)) >= 0) {
        diatonic_chord(key, par, d, ext, out);
        *flats = key_uses_flats(key, par);
        return 0;
    }
    int neapolitan = ((pc - key) % 12 + 12) % 12 == 1;
    *flats = neapolitan;
    static const char *const NEAP[NUM_EXTS] = {"", "maj7", "maj9", "sus2", "sus4"};
    static const char *const DIM[NUM_EXTS] = {"dim", "dim7", "dim7b9", "dim", "dim"};
    chord_from_quality(pc, quality_index(neapolitan ? NEAP[ext < 0 || ext >= NUM_EXTS ? 0 : ext]
                                                    : DIM[ext < 0 || ext >= NUM_EXTS ? 0 : ext]), out);
    return 0;
}

void vary_chord(const chord_t *c, int key, int scale, chord_t *out) {
    *out = *c;
    if (c->n <= 0) return;
    const quality_t *q = NULL;
    int iv[MAX_TONES];
    if (c->n < 5) {
        /* add the next tone: a 7th to a triad, a 9th to a 7th chord; in-scale candidates first */
        static const int SEVENTH[] = {11, 10, 9}, NINTH[] = {14, 13, 15};
        const int *cand = c->n == 3 ? SEVENTH : NINTH;
        int sus = c->n == 3 && c->iv[1] == 5;   /* a sus4 takes its b7 even off the scale (7sus4, not maj7sus4) */
        memcpy(iv, c->iv, c->n * sizeof(int));
        if (sus) {
            iv[3] = 10;
            q = match_quality(4, iv);
        }
        for (int pass = 0; pass < 2 && !q; pass++)
            for (int i = 0; i < 3 && !q; i++) {
                if (pass == 0 && scale_degree(key, scale, (c->root + cand[i]) % 12) < 0) continue;
                iv[c->n] = cand[i];
                q = match_quality(c->n + 1, iv);
            }
    }
    if (!q) q = match_quality(3, c->iv);   /* 9ths and up, or nothing fits: back to the triad */
    if (!q) return;
    out->n = q->n;
    out->quality = q->name;
    memcpy(out->iv, q->iv, sizeof out->iv);
}

int parse_chord(int key, const char *tok, int len, chord_t *out) {
    static const struct { const char *r; int semis; } ROMAN[] = {
        {"vii", 11}, {"vi", 9}, {"v", 7}, {"iv", 5}, {"iii", 4}, {"ii", 2}, {"i", 0},
    };
    int i = 0, acc = 0;
    memset(out, 0, sizeof *out);
    if (i < len && (tok[i] == 'b' || tok[i] == '#')) acc = tok[i++] == 'b' ? -1 : 1;
    if (i >= len) return 0;
    int upper = tok[i] >= 'A' && tok[i] <= 'Z', semis = -1;
    for (int r = 0; r < 7 && semis < 0; r++) {
        int rl = (int)strlen(ROMAN[r].r);
        if (i + rl > len) continue;
        int ok = 1;
        for (int k = 0; k < rl && ok; k++) ok = (tok[i + k] | 0x20) == ROMAN[r].r[k];
        /* "iv" must not match the "i" of "ii"/"iii"; longest first plus this boundary check */
        if (ok && i + rl < len && tok[i + rl] != ':') ok = 0;
        if (ok) { semis = ROMAN[r].semis; i += rl; }
    }
    if (semis < 0) return 0;
    const quality_t *q;
    if (i < len && tok[i] == ':') q = find_quality(tok + i + 1, len - i - 1);
    else q = find_quality(upper ? "" : "m", upper ? 0 : 1);
    if (!q) return 0;
    out->root = ((key + semis + acc) % 12 + 12) % 12;
    set_quality(out, q);
    return 1;
}

int set_chord(int key, int set, int slot, chord_t *out) {
    memset(out, 0, sizeof *out);
    if (set < 0 || set >= NUM_CHORD_SETS || slot < 0) return 0;
    const char *p = CHORD_SETS[set].chords;
    for (int k = 0; *p; k++) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e == p) break;
        if (k == slot) return parse_chord(key, p, (int)(e - p), out);
        p = e;
    }
    return 0;
}

const char *pc_name(int pc, int flats) {
    static const char *const SHARP[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char *const FLAT[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    pc = ((pc % 12) + 12) % 12;
    return flats ? FLAT[pc] : SHARP[pc];
}

int key_uses_flats(int key, int scale) {
    if (scale < 0 || scale >= NUM_SCALES) scale = 0;
    int major = (key + PARENT_MAJOR[scale]) % 12;
    return major == 5 || major == 10 || major == 3 || major == 8 || major == 1;   /* F Bb Eb Ab Db */
}

void chord_name(const chord_t *c, int flats, char *buf, int len) {
    if (!c || c->n <= 0) snprintf(buf, len, "-");
    else snprintf(buf, len, "%s%s", pc_name(c->root, flats), c->quality ? c->quality : "");
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* close/drop/spread shape of c at one inversion and octave shift, before the bass note */
static void shape(const chord_t *c, int voicing, int inv, int root_midi, voicing_t *v) {
    v->n = c->n;
    for (int i = 0; i < c->n; i++) v->note[i] = root_midi + c->iv[i];
    inv %= c->n;
    for (int i = 0; i < inv; i++) v->note[i] += 12;
    qsort(v->note, v->n, sizeof(int), cmp_int);
    if (voicing == VOICE_DROP2 && v->n >= 3) v->note[v->n - 2] -= 12;
    else if (voicing == VOICE_DROP3 && v->n >= 4) v->note[v->n - 3] -= 12;
    else if (voicing == VOICE_DROP3 && v->n == 3) v->note[v->n - 2] -= 12;
    else if (voicing == VOICE_SPREAD && v->n >= 3)
        for (int i = 1; i < v->n; i += 2) v->note[i] += 12;
    qsort(v->note, v->n, sizeof(int), cmp_int);
}

static int nearest(int x, const voicing_t *v) {
    int best = 1000;
    for (int i = 0; i < v->n; i++) {
        int d = abs(x - v->note[i]);
        if (d < best) best = d;
    }
    return best;
}

/* movement between two voicings, both ways so a dropped or added note counts */
static int distance(const voicing_t *a, const voicing_t *b) {
    int d = 0;
    for (int i = 0; i < a->n; i++) d += nearest(a->note[i], b);
    for (int i = 0; i < b->n; i++) d += nearest(b->note[i], a);
    return d;
}

static int center(const voicing_t *v) {
    int s = 0;
    for (int i = 0; i < v->n; i++) s += v->note[i];
    return v->n ? s / v->n : 0;
}

void voice_chord(const chord_t *c, const voice_opts_t *o, const voicing_t *prev, voicing_t *out) {
    memset(out, 0, sizeof *out);
    if (!c || c->n <= 0) return;
    int oct = o->octave < -2 ? -2 : o->octave > 2 ? 2 : o->octave;
    int root_midi = 48 + c->root + 12 * oct;
    voicing_t best;
    shape(c, o->voicing, o->inversion < 0 ? 0 : o->inversion, root_midi, &best);
    if (o->voice_lead && prev && prev->n > 0) {
        /* the chord stays near the octave setting's register, so leading can't walk off the keyboard */
        int target = 60 + 12 * oct, best_cost = 1 << 30;
        voicing_t prev_upper = *prev;
        if (o->bass && prev_upper.n > 1) {   /* lead the upper voices only; the bass is added below */
            prev_upper.n--;
            memmove(prev_upper.note, prev_upper.note + 1, prev_upper.n * sizeof(int));
        }
        for (int inv = 0; inv < c->n; inv++)
            for (int sh = -12; sh <= 12; sh += 12) {
                voicing_t v;
                shape(c, o->voicing, inv, root_midi + sh, &v);
                int cost = 2 * distance(&v, &prev_upper) + abs(center(&v) - target);
                if (cost < best_cost) { best_cost = cost; best = v; }
            }
    }
    if (o->bass) {
        int b = best.note[0] - 12;
        b -= ((b - c->root) % 12 + 12) % 12;
        if (b >= 0 && best.n < MAX_VOICE) {
            memmove(best.note + 1, best.note, best.n * sizeof(int));
            best.note[0] = b;
            best.n++;
        }
    }
    for (int i = 0; i < best.n; i++)
        if (best.note[i] >= 0 && best.note[i] <= 127 && (out->n == 0 || out->note[out->n - 1] != best.note[i]))
            out->note[out->n++] = best.note[i];
}

/* ---- button chords ---- */
static const char *const BTYPE_NAME[NUM_BTYPES] = {"", "dim", "m", "", "sus4"};
static const int BTYPE_IV[NUM_BTYPES][3] = {{0, 4, 7}, {0, 3, 6}, {0, 3, 7}, {0, 4, 7}, {0, 5, 7}};

int button_chord(int key, int scale, int pc, int type, int ext, chord_t *out, char *qbuf, int qlen, int *flats) {
    chord_t tri, sev;
    int fl = key_uses_flats(key, scale), diatonic = 1, iv[MAX_TONES], n = 0;
    if (!flats) flats = &fl;
    *flats = fl;
    pc = ((pc % 12) + 12) % 12;
    if (type < 0 || type >= NUM_BTYPES) type = BT_AUTO;
    memset(out, 0, sizeof *out);
    out->root = pc;
    if (type == BT_AUTO) {
        diatonic = key_chord(key, scale, pc, EXT_TRIAD, &tri, flats);
        if (!(ext & 15)) { *out = tri; return diatonic; }
        for (int i = 0; i < 3 && i < tri.n; i++) iv[n++] = tri.iv[i];
    } else
        for (int i = 0; i < 3; i++) iv[n++] = BTYPE_IV[type][i];
    int seventh = 0;
    if (ext & BX_9 && !(ext & (BX_6 | BX_M7 | BX_MAJ7))) {   /* a 9 alone brings its 7th: the key's, else b7 */
        if (type == BT_AUTO) {
            key_chord(key, scale, pc, EXT_7TH, &sev, NULL);
            seventh = sev.n >= 4 ? sev.iv[3] : 10;
        } else
            seventh = type == BT_MAJ ? 11 : 10;
    }
    int add[5] = {ext & BX_6 ? 9 : 0, ext & BX_M7 ? 10 : 0, ext & BX_MAJ7 ? 11 : 0, seventh, ext & BX_9 ? 14 : 0};
    for (int a = 0; a < 5; a++) {
        if (!add[a]) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) dup |= iv[i] == add[a];
        if (!dup && n < MAX_TONES) iv[n++] = add[a];
    }
    for (int i = 1; i < n; i++)   /* ascending */
        for (int j = i; j > 0 && iv[j] < iv[j - 1]; j--) { int t = iv[j]; iv[j] = iv[j - 1]; iv[j - 1] = t; }
    out->n = n;
    memcpy(out->iv, iv, n * sizeof(int));
    for (int q = 0; q < NUM_QUALITIES; q++)
        if (QUALITIES[q].n == n && !memcmp(QUALITIES[q].iv, iv, n * sizeof(int))) {
            out->quality = QUALITIES[q].name;
            return diatonic;
        }
    /* nothing in the table: the triad's name and what was added, "m(6,maj7)" */
    const char *base = type == BT_AUTO ? (iv[1] == 3 ? (iv[2] == 6 ? "dim" : "m") : iv[1] == 5 ? "sus4" : iv[2] == 8 ? "aug" : "")
                                       : BTYPE_NAME[type];
    int len = snprintf(qbuf, qlen, "%s(", base), first = 1;
    static const char *const XN[4] = {"6", "7", "maj7", "9"};
    for (int b = 0; b < 4; b++)
        if (ext & (1 << b) && len < qlen) { len += snprintf(qbuf + len, qlen - len, "%s%s", first ? "" : ",", XN[b]); first = 0; }
    if (len < qlen) snprintf(qbuf + len, qlen - len, ")");
    out->quality = qbuf;
    return diatonic;
}
