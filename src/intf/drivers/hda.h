// hda.h - Intel High Definition Audio: the laptop's speakers and headphones
//
// The HDA controller (in the chipset) talks to codec chips (Realtek and the
// like) over a serial link: it sends them commands ("verbs") through a ring
// in memory (CORB) and reads their answers from another (RIRB). A codec is a
// graph of widgets: converters (DACs), mixers, selectors and pins (the jacks
// and the internal speaker). The driver walks the graph from the speaker and
// headphone pins back to a DAC, powers and unmutes everything on the way,
// and then streams 48 kHz 16-bit stereo samples to the DAC by DMA.
#ifndef HDA_H
#define HDA_H

#include <stdint.h>

/* Find the controller and a codec with an output; set up the path.
 * Returns 0 if sound can be played. `how` describes the result. */
int sound_init(char *how, int size);

int sound_ready(void);

/* `sound`: controller, codecs, widgets on the chosen paths. */
void sound_print_info(void);

/* Volume 0..100 (software gain); sound_get_volume() for the shell. */
void sound_set_volume(int percent);
int sound_get_volume(void);

/* A sine tone. Returns 0 when played, -1 on error, 1 if stopped (Ctrl+C). */
int sound_beep(uint32_t hz, uint32_t ms);

/* A WAV file (PCM, 8 or 16 bit, mono or stereo, any rate) already in
 * memory. Same return values; *error says why it cannot be played. */
int sound_play_wav(const uint8_t *data, uint32_t size, const char **error);

#endif
