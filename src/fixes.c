// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
// Turn invalid fixes into a valid fix sha.
//
// We read in a list that we have pre-determined is the correct mapping,
// and then use that when reading "Fixes:" tags from the kernel repo.
//

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "verhaal.h"
#include "terminal.h"

#define NUM_FIXES	3000
#define FIXES_FILE	"fixes.txt"

// Current sha1 is only 40 chars, but be safe and pick 64 for now
struct fixes {
	char sha_invalid[64];
	char sha_valid[64];
};

// Stupid dumb simple array for all of the fixes.  Should be fast enough, if not, we can use
// something else later on.  And abuse "free" memory by just allocating it statically in a big
// chunk all at once.

static struct fixes fixes_array[NUM_FIXES];
static int max_fixes;

static void add_fix_pair(const char *invalid, const char *valid)
{
	struct fixes *fix;

	fix = &fixes_array[max_fixes];
	strcpy(fix->sha_invalid, invalid);
	strcpy(fix->sha_valid, valid);
	++max_fixes;

	db_fix_add(invalid, valid);
	//printf("%s: invalid: %s	valid: %s\n", __func__, invalid, valid);
}

char *fix_translate(const char *fix_sha)
{
	char *new_fix = NULL;
	int ret;
	int i;

	for (i = 0; i < max_fixes; ++i) {
		struct fixes *fix = &fixes_array[i];

		// FIXME: see if we have searched too far now, the list was sorted...
		ret = strcmp(fix_sha, fix->sha_invalid);
		if (ret == 0) {
			new_fix = strdup(fix->sha_valid);
			goto exit;
		}
	}

exit:
	return new_fix;
}

void fixes_init(void)
{
	struct vh_timestamp *foo;
	FILE *fixes_file;
	double seconds;
	size_t size;
	size_t read;
	char *buffer;

	// No need to re-parse the fixes stuff if we are not doing this "from scratch"
	if (!db_is_in_memory)
		return;

	foo = time_start(__func__);

	fixes_file = fopen(FIXES_FILE, "r");
	if (!fixes_file) {
		fprintf(stderr, "Error: %s is not able to be opened\n", FIXES_FILE);
		return;
	}

	// Find the size of the file so we can suck it all in at once
	fseek(fixes_file, 0, SEEK_END);
	size = ftell(fixes_file);

	buffer = malloc(size);
	if (!buffer)
		goto exit;

	// Go back to the front and read it all in
	fseek(fixes_file, 0, SEEK_SET);
	read = fread(buffer, 1, size, fixes_file);
	if (read != size) {
		fprintf(stderr, "Error: %zd bytes read from a file of size %zd\n", read, size);
		goto exit;
	}

	char *line = strtok(buffer, "\n");
	while (line) {
		if (line[0] != '#') {
			char bad[100], good[100];
			int num = sscanf(line, "%s %s", bad, good);
			if (num == 2) {
				if (strcmp(good, "NOT_VALID") != 0) {
					add_fix_pair(bad, good);
				}
			}
		}
		line = strtok(NULL, "\n");
	}

	free(buffer);

exit:
	fclose(fixes_file);
	seconds = time_stop(foo);
	terminal_fprintf(stdout, "    Fixes parsing took took "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", seconds);
}

