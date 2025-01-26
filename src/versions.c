// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "verhaal.h"


#define NUM_VERSIONS	2000	// Good for a few more years...
#define NAME_SIZE	20	// should fit the whole vX.Y.Z string size

struct version {
	char name[NAME_SIZE];
	bool mainline;
};

struct version version_array[NUM_VERSIONS];



