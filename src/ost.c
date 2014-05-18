/* Onset Detector
 *
 * Copyright (C) 2014 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <stdbool.h>

/* LV2 */
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"
#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/midi/midi.h"


#ifndef MIN
#define MIN(A,B) ( (A) < (B) ? (A) : (B) )
#endif
#ifndef MAX
#define MAX(A,B) ( (A) > (B) ? (A) : (B) )
#endif

#include "spectr.c"

/******************************************************************************
 * LV2 routines
 */

#define OST_URI "http://gareus.org/oss/lv2/onsettrigger#"

typedef enum {
	OST_MIDI_OUT,
	OST_LATENCY,
	OST_MIDI_NOTE,
	OST_AIN_1,
	OST_AIN_2,
} PortIndex;


typedef struct {
	/* Input Ports */
	float* a_in[2];
	float* m_note;
	float* p_latency;

	/* MIDI Out */
	LV2_Atom_Sequence* midiout;
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Frame frame;
	LV2_URID_Map* map;
	LV2_URID midi_MidiEvent;
	LV2_URID atom_Sequence;

	/* internal state */
	struct FilterBank fb;
	uint32_t midi_note_off_timeout;
	float rms_postfilter;

	/* config */
	double rate;
	uint32_t n_channels;
	uint32_t midi_note_off_cfg;
	float rms_omega;

} OST;

static LV2_Handle
instantiate(
		const LV2_Descriptor*     descriptor,
		double                    rate,
		const char*               bundle_path,
		const LV2_Feature* const* features)
{
	OST* self = (OST*)calloc(1, sizeof(OST));
	if(!self) {
		return NULL;
	}

	int i;
	for (i=0; features[i]; ++i) {
		if (!strcmp(features[i]->URI, LV2_URID__map)) {
			self->map = (LV2_URID_Map*)features[i]->data;
		}
	}

	if (!self->map) {
		fprintf(stderr, "OnsetTrigger.lv2 error: Host does not support urid:map\n");
		free(self);
		return NULL;
	}

	if (!strncmp(descriptor->URI, OST_URI "bassdrum_mono", 39 + 13 )) {
		self->n_channels = 1;
	} else if (!strncmp(descriptor->URI, OST_URI "bassdrum_stereo", 39 + 15 )) {
		self->n_channels = 2;
	} else {
		fprintf(stderr, "OnsetTrigger.lv2 error: invalid plugin variant given\n");
		free(self);
		return NULL;
	}

	self->midi_MidiEvent = self->map->map(self->map->handle, LV2_MIDI__MidiEvent);
	self->atom_Sequence  = self->map->map(self->map->handle, LV2_ATOM__Sequence);
	lv2_atom_forge_init(&self->forge, self->map);

	/* config */
	self->midi_note_off_cfg = MAX(1, .05 * rate)  ;
	self->rate = rate;
	self->rms_omega = 1.0f - expf(-2.0 * M_PI * 15.0 / rate);

	/* state */
	self->rms_postfilter = 0;
	self->midi_note_off_timeout = 0;
	bandpass_setup(&self->fb, self->rate, 120, 50, 2);

	return (LV2_Handle)self;
}

static void
connect_port(LV2_Handle handle,
               uint32_t   port,
               void*      data)
{
	OST* self = (OST*)handle;

	switch ((PortIndex)port) {
		case OST_MIDI_OUT:
			self->midiout = (LV2_Atom_Sequence*)data;
			break;
		case OST_AIN_1:
			self->a_in[0]  = (float*)data;
			break;
		case OST_AIN_2:
			self->a_in[1]  = (float*)data;
			break;
		case OST_LATENCY:
			self->p_latency = (float*)data;
			break;
		case OST_MIDI_NOTE:
			self->m_note = (float*)data;
			break;
	}
}


static void midi_tx(OST *self, int64_t tme, uint8_t raw_midi[3])
{
	LV2_Atom midiatom;
	midiatom.type = self->midi_MidiEvent;
	midiatom.size = 3;
	lv2_atom_forge_frame_time(&self->forge, tme);
	lv2_atom_forge_raw(&self->forge, &midiatom, sizeof(LV2_Atom));
	lv2_atom_forge_raw(&self->forge, raw_midi, 3);
	lv2_atom_forge_pad(&self->forge, sizeof(LV2_Atom) + midiatom.size);
}

static void midi_note(OST *self, int64_t tme, uint8_t velocity)
{
	uint8_t raw_midi[3];
	const uint8_t channel = 0; // TODO
	raw_midi[0] = (channel & 0x0f) | ((velocity & 0x7f) ? 0x90 : 0x80);
	raw_midi[1] = (uint8_t)(*self->m_note) & 0x7f;
	raw_midi[2] = velocity & 0x7f;
	midi_tx(self, tme, raw_midi);
}

static void
run(LV2_Handle handle, uint32_t n_samples)
{
	OST* self = (OST*)handle;

	/* localize variables */
	float const * const a_in = self->a_in[0];
	float rms_postfilter = self->rms_postfilter;
	float rms_postfilter_z = self->rms_postfilter;
	uint32_t midi_note_off_timeout = self->midi_note_off_timeout;
	const float rms_omega  = self->rms_omega;

	*self->p_latency = .017 * self->rate;

	if (n_samples == 0 || ! self->midiout) {
		return;
	}

	const uint32_t capacity = self->midiout->atom.size;
	lv2_atom_forge_set_buffer(&self->forge, (uint8_t*)self->midiout, capacity);
	lv2_atom_forge_sequence_head(&self->forge, &self->frame, 0);

	for (uint32_t n = 0 ; n < n_samples; ++n) {
		const float signal = bandpass_process(&self->fb, a_in[n]);
		rms_postfilter_z = rms_postfilter;
		rms_postfilter += rms_omega * ( (signal * signal) - rms_postfilter) + 1e-20;

		/* quick rms+time based hack to do something */
		if (midi_note_off_timeout > 0) {
			if (--midi_note_off_timeout == 0) {
				midi_note(self, n, 0);
			}
		} else if (rms_postfilter > .15 && rms_postfilter_z < rms_postfilter) {
			midi_note_off_timeout = self->midi_note_off_cfg;
			//printf("TRIGGER! %.2f\n", rms_postfilter);
			midi_note(self, n, 0x7f);
		}
	}

	/* copy back variables */
	self->rms_postfilter = rms_postfilter;
	self->midi_note_off_timeout = midi_note_off_timeout;
	//lv2_atom_forge_pop(&self->forge, &self->frame);
}

static void
cleanup(LV2_Handle handle)
{
	//OST* self = (OST*)handle;
	free(handle);
}


/******************************************************************************
 * LV2 setup
 */

const void*
extension_data(const char* uri)
{
	return NULL;
}


#define mkdesc_osc(ID, NAME) \
static const LV2_Descriptor descriptor ## ID = { \
	OST_URI NAME,   \
	instantiate,    \
	connect_port,   \
	NULL,           \
	run,            \
	NULL,           \
	cleanup,        \
	extension_data  \
};

mkdesc_osc(0, "bassdrum_mono");
mkdesc_osc(1, "bassdrum_stereo");

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
	switch (index) {
		case  0: return &descriptor0;
		case  1: return &descriptor1;
		default: return NULL;
	}
}

/* vi:set ts=2 sts=2 sw=2: */
