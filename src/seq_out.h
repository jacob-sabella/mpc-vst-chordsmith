/* MIDI out through an ALSA sequencer port. MPC OS ignores VST MIDI output, so the chords leave the
 * plugin here and another track picks the port as its MIDI input (mpc-vst-plugins docs/NOTES.md,
 * "MIDI-output plugins"). libasound is dlopen'd, so the plugin builds without ALSA headers and still
 * loads (with no MIDI out) where the library is missing. One port is shared by every instance. */
#pragma once

int seq_out_acquire(void);          /* 1 if the port is up; call once per instance */
void seq_out_release(void);
void seq_out_note(int ch, int note, int vel);   /* vel 0 = note off; never blocks */
const char *seq_out_status(void);   /* short text for the skin */

/* Tests: capture events instead of sending them (call before the first acquire). */
typedef void (*seq_out_sink_t)(int ch, int note, int vel);
void seq_out_set_sink(seq_out_sink_t sink);
