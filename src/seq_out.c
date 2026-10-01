#include "seq_out.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define CLIENT_NAME "Chordsmith"
#define PORT_NAME "MIDI Out"

/* The parts of alsa/asoundlib.h used here. This ABI (snd_seq_event_t is 28 bytes) has not changed
 * since ALSA 1.0; tests/theory_test.c checks it against the real header where one is installed. */
typedef struct snd_seq snd_seq_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    struct { unsigned int sec, nsec; } time;
    struct { unsigned char client, port; } source, dest;
    union {
        struct { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } note;
        unsigned char raw8[12];
    } data;
} seq_event_t;
_Static_assert(sizeof(seq_event_t) == 28, "snd_seq_event_t layout");

enum {
    SEQ_OPEN_OUTPUT = 1, SEQ_NONBLOCK = 1,
    EV_NOTEON = 6, EV_NOTEOFF = 7,
    ADDR_SUBSCRIBERS = 254, ADDR_UNKNOWN = 253, QUEUE_DIRECT = 253,
    CAP_READ = 1 << 0, CAP_SUBS_READ = 1 << 5,
    TYPE_MIDI_GENERIC = 1 << 1, TYPE_APPLICATION = 1 << 20,
};

static struct {
    int (*open)(snd_seq_t **, const char *, int, int);
    int (*set_client_name)(snd_seq_t *, const char *);
    int (*create_simple_port)(snd_seq_t *, const char *, unsigned int, unsigned int);
    int (*output_direct)(snd_seq_t *, seq_event_t *);
    int (*close)(snd_seq_t *);
} A;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *lib;
static snd_seq_t *seq;
static int port = -1, users;
static const char *status = "MIDI OUT: OFF";
static seq_out_sink_t sink;

void seq_out_set_sink(seq_out_sink_t s) { sink = s; }

static int load(void) {
    if (lib) return 1;
    lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
    *(void **)&A.open = dlsym(lib, "snd_seq_open");
    *(void **)&A.set_client_name = dlsym(lib, "snd_seq_set_client_name");
    *(void **)&A.create_simple_port = dlsym(lib, "snd_seq_create_simple_port");
    *(void **)&A.output_direct = dlsym(lib, "snd_seq_event_output_direct");
    *(void **)&A.close = dlsym(lib, "snd_seq_close");
    if (A.open && A.set_client_name && A.create_simple_port && A.output_direct && A.close) return 1;
    dlclose(lib);
    lib = NULL;
    return 0;
}

int seq_out_acquire(void) {
    pthread_mutex_lock(&lock);
    users++;
    if (sink) status = "MIDI OUT: TEST";
    else if (!seq) {
        if (!load()) status = "MIDI OUT: NO ALSA";
        else if (A.open(&seq, "default", SEQ_OPEN_OUTPUT, SEQ_NONBLOCK) < 0) { seq = NULL; status = "MIDI OUT: NO SEQUENCER"; }
        else {
            A.set_client_name(seq, CLIENT_NAME);
            port = A.create_simple_port(seq, PORT_NAME, CAP_READ | CAP_SUBS_READ, TYPE_MIDI_GENERIC | TYPE_APPLICATION);
            if (port < 0) { A.close(seq); seq = NULL; status = "MIDI OUT: NO PORT"; }
            else status = "MIDI OUT: PORT READY";   /* value text is cut at 23 characters */
        }
    }
    int ok = sink || seq;
    pthread_mutex_unlock(&lock);
    return ok;
}

void seq_out_release(void) {
    pthread_mutex_lock(&lock);
    if (users > 0 && --users == 0 && seq) {
        A.close(seq);
        seq = NULL;
        port = -1;
        status = "MIDI OUT: OFF";
    }
    pthread_mutex_unlock(&lock);
}

/* Audio thread. No lock: seq only changes when the first instance is created or the last destroyed. */
void seq_out_note(int ch, int note, int vel) {
    if (sink) { sink(ch, note, vel); return; }
    if (!seq || note < 0 || note > 127) return;
    seq_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = vel > 0 ? EV_NOTEON : EV_NOTEOFF;
    ev.queue = QUEUE_DIRECT;
    ev.source.port = (unsigned char)port;
    ev.dest.client = ADDR_SUBSCRIBERS;
    ev.dest.port = ADDR_UNKNOWN;
    ev.data.note.channel = (unsigned char)(ch & 15);
    ev.data.note.note = (unsigned char)note;
    ev.data.note.velocity = (unsigned char)(vel > 127 ? 127 : vel < 0 ? 0 : vel);
    A.output_direct(seq, &ev);
}

const char *seq_out_status(void) { return status; }
