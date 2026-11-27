/*
 * The screen's brightness: the backlight under /sys/class/backlight. Its
 * brightness file is writable for the group video (a udev rule in
 * wormos-desktop), which desktop users are in. With several backlights,
 * the kernel's preference order is used: firmware, then platform, then raw.
 */

#include <stdio.h>
#include <string.h>

#include "shell.h"

#define BACKLIGHTS "/sys/class/backlight"
/* Never all the way down: a black screen looks broken. */
#define MIN_LEVEL  0.05

static char device[256];        /* the backlight's folder, or "" */
static gboolean looked;

static long read_number(const char *dir, const char *file)
{
	char path[512];
	long value = -1;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	if ((f = fopen(path, "re"))) {
		if (fscanf(f, "%ld", &value) != 1)
			value = -1;
		fclose(f);
	}
	return value;
}

static void find_device(void)
{
	static const char *const order[] = { "firmware", "platform", "raw" };
	GDir *dir;
	const char *name;
	int best = G_N_ELEMENTS(order);

	looked = TRUE;
	device[0] = '\0';
	if (!(dir = g_dir_open(BACKLIGHTS, 0, NULL)))
		return;
	while ((name = g_dir_read_name(dir))) {
		char path[512], *type = NULL;
		int rank = G_N_ELEMENTS(order) - 1;

		snprintf(path, sizeof(path), BACKLIGHTS "/%s/type", name);
		if (g_file_get_contents(path, &type, NULL, NULL)) {
			g_strstrip(type);
			for (guint i = 0; i < G_N_ELEMENTS(order); i++)
				if (!strcmp(type, order[i]))
					rank = (int)i;
			g_free(type);
		}
		if (rank < best || !device[0]) {
			best = rank;
			snprintf(device, sizeof(device), BACKLIGHTS "/%s", name);
		}
	}
	g_dir_close(dir);
}

gboolean brightness_available(void)
{
	if (!looked)
		find_device();
	return device[0] && read_number(device, "max_brightness") > 0;
}

/* 0 to 1, or -1 without a backlight. */
double brightness_get(void)
{
	long max, now;

	if (!brightness_available())
		return -1;
	max = read_number(device, "max_brightness");
	now = read_number(device, "brightness");
	return now < 0 ? -1 : (double)now / (double)max;
}

void brightness_set(double level)
{
	char path[512];
	long max;
	FILE *f;

	if (!brightness_available())
		return;
	level = CLAMP(level, MIN_LEVEL, 1.0);
	max = read_number(device, "max_brightness");
	snprintf(path, sizeof(path), "%s/brightness", device);
	if ((f = fopen(path, "we"))) {
		fprintf(f, "%ld\n", (long)(level * (double)max + 0.5));
		fclose(f);
	}
}

/* Up (1) or down (-1) by a tenth. */
void brightness_step(int direction)
{
	double now = brightness_get();

	if (now >= 0)
		brightness_set(now + 0.1 * direction);
}
