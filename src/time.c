// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
// Simple library to do timestamp of a start/stop sequence
//
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "verhaal.h"


struct vh_timestamp {
	char *name;
	struct timespec tv;
};

struct vh_timestamp *time_start(const char *name)
{
	struct vh_timestamp *start;

	start = malloc(sizeof(*start));
	if (!start)
		return NULL;

	start->name = strdup(name);
	if (!start->name) {
		free(start);
		return NULL;
	}

	if (clock_gettime(CLOCK_REALTIME, &start->tv)) {
		fprintf(stderr, "%s: error getting time\n", __func__);
		return NULL;
	}
	return start;
}

void time_stop(struct vh_timestamp *start)
{
	struct timespec stop;
	double seconds;

	if (!start)
		return;

	if (clock_gettime(CLOCK_REALTIME, &stop)) {
		fprintf(stderr, "%s: error getting time\n", __func__);
		return;
	}

	seconds = ((double)stop.tv_sec + (1.0e-9 * stop.tv_nsec)) -
		  ((double)start->tv.tv_sec + (1.0e-9 * start->tv.tv_nsec));

	printf("%s: %.5f seconds\n", start->name, seconds);
	free(start);
}
