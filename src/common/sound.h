/*
 * The sound outputs and inputs a person would choose between: each
 * connector of each sound device (speakers, headphones, a microphone,
 * HDMI), not PipeWire's nodes, with plain names. Read from pw-dump.
 */

#ifndef WORMOS_SOUND_H
#define WORMOS_SOUND_H

#include <glib.h>

struct sound_choice {
	char *label;            /* "Speakers", "Headphones", "Built-in microphone"... */
	gboolean output;        /* or an input */
	gboolean active;        /* what plays (or records) now */
	int node;               /* its PipeWire node, or -1 (another profile) */
	int device;             /* the PipeWire device, or -1 (a node of its own) */
	int route, route_device, profile;
	double volume;          /* 0 to 1, or -1 */
};

/* The choices, outputs first (a GPtrArray of struct sound_choice), or NULL
 * without a sound server. *signature (if given) changes when the list or
 * what is active does. */
GPtrArray *sound_choices(char **signature);

/* Make this choice play (or record): its profile and connector on its
 * device, then the default. Waits up to two seconds for a profile change. */
void sound_choose(const struct sound_choice *c);

/* A plain name for a sink or source description ("... Controller Speaker"
 * -> "Speakers"); free it. */
char *sound_plain_name(const char *description, gboolean output);

#endif
