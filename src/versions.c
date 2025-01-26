// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <git2.h>
#include "verhaal.h"


#define NUM_VERSIONS	20000	// Good for a few more years...
#define NAME_SIZE	20	// should fit the whole vX.Y.Z string size

struct version {
	char name[NAME_SIZE];
	bool mainline;
};

static struct version version_array[NUM_VERSIONS];
static int max_version;

static bool is_valid_release(const char *version)
{
	int ret;
	git_object *obj = NULL;

	ret = git_revparse_single(&obj, git_repo, version);
	if (ret)
		return false;
	git_object_free(obj);
	return true;
}

static void add_version(const char *version, bool mainline)
{
	struct version *v = &version_array[max_version];

	strcpy(v->name, version);
	v->mainline = mainline;

	//printf("%d	%s	%d\n", max_version, version, mainline);
	max_version++;
}

static void add_version_major(const char *version)
{
	add_version(version, true);
}

static void add_version_minor(const char *version)
{
	add_version(version, false);
}

static void loop_through_2(void)
{
	char str[256];

	for (int x = 1; x < 41; ++x) {
		snprintf(str, sizeof(str), "v2.6.%d", x);
		if (!is_valid_release(str)) {
			dbg("%s is NOT a valid release\n", str);
			continue;
		}

		snprintf(str, sizeof(str), "2.6.%d", x);
		add_version_major(str);

		for (int y = 1; y < 101; ++y) {
			snprintf(str, sizeof(str), "v2.6.%d.%d", x, y);
			if (!is_valid_release(str)) {
				dbg("%s is NOT a valid release\n", str);
				continue;
			}

			snprintf(str, sizeof(str), "2.6.%d.%d", x, y);
			add_version_minor(str);
		}
	}
}

static void loop_through_x(int major)
{
	char str[256];
	int minor;
	int y;

	for (minor = 0; minor < 40; ++minor) {
		snprintf(str, sizeof(str), "v%d.%d", major, minor);
		if (!is_valid_release(str)) {
			dbg("%s is NOT a valid release\n", str);
			continue;
		}

		snprintf(str, sizeof(str), "%d.%d", major, minor);
		add_version_major(str);

		for (y = 1; y < 400; ++y) {
			snprintf(str, sizeof(str), "v%d.%d.%d", major, minor, y);
			if (!is_valid_release(str)) {
				dbg("%s is NOT a valid release\n", str);
				continue;
			}

			snprintf(str, sizeof(str), "%d.%d.%d", major, minor, y);
			add_version_minor(str);
		}
	}

}

void versions_create(void)
{
	struct vh_timestamp *foo = time_start(__func__);
	loop_through_2();
	loop_through_x(3);
	loop_through_x(4);
	loop_through_x(5);
	loop_through_x(6);
	time_stop(foo);
}
