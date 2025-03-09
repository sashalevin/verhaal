// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
// Note, this is a duplicate of the existing logic in main.c to cycle
// through all of the versions to create the ranges.  For now, just use this
// list to populate the database and hopefully, eventually, track what versions
// are, and are not, in the database so we can create the "remaining ranges" to
// build so we don't have to scan the whole world each time this program runs.
//
// Also, we do NOT handle the -rc calculations here, that's still in main.c.
// Should be moved here eventually as well.

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <git2.h>
#include "verhaal.h"
#include "terminal.h"

#define NUM_VERSIONS	20000	// Good for a few more years...
#define NAME_SIZE	20	// should fit the whole vX.Y.Z string size

/* A specific version, and if it is in mainline or not */
struct version {
	char name[NAME_SIZE];
	bool mainline;
};

/*
 * Version ranges are the steps from one release to another, the granularity in
 * which we want to calculate commits in.  While we keep the individual release
 * versions in the database to lookup mainline/not_mainline info from, it is
 * these "ranges" that matter in how we spelunk through git and save git ids
 */
struct version_range {
	struct version v_from;
	struct version v_to;
	bool mainline;
};

/* Yes, we could use a vector, or linked list, but hey, this is userspace, we
 * have a ton of memory, just use a simple array and be done with it.
 */
static struct version version_array[NUM_VERSIONS];
static int max_version;

static struct version_range version_range_array[NUM_VERSIONS];
static int max_version_range;

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
	if (max_version > NUM_VERSIONS) {
		fprintf(stderr, "Number of versions just overflowed, fix NUM_VERSIONS to be bigger!\n");
		exit(1);
	}

	// Add the version to the database
	db_release_add(version, mainline);
}

static void add_version_major(const char *version)
{
	add_version(version, true);
}

static void add_version_minor(const char *version)
{
	add_version(version, false);
}

static void add_version_range(const char *from, const char *to, bool mainline)
{
	struct version_range *vr = &version_range_array[max_version_range];

	// Let's first see if these are a few "known" ranges that we know we can
	// never find, thanks to the start of the git repo and how the first few
	// tags were set up.
	// (i.e. v2.6.11 is NOT a real commit, but rather a tree.)
	if ((strcmp(from, "2.6.11") == 0) || (strcmp(to, "2.6.11") == 0)) {
		dbg("skipping invalid 2.6.11 commit as that's just a mess\n");
		return;
	}
	if ((strcmp(to, "3.16.35") == 0)) {
		dbg("skipping invalid 3.16.35 commit as there was a 'break' there\n");
		return;
	}

	strcpy(vr->v_from.name, from);
	strcpy(vr->v_to.name, to);
	vr->mainline = mainline;

	//printf("%s: from: %s	to: %s	mainline: %d\n", __func__, from, to, mainline);
	max_version_range++;
}

static void add_version_range_major(const char *major, const char *minor)
{
	add_version_range(major, minor, true);
}

static void add_version_range_minor(const char *major, const char *minor)
{
	add_version_range(major, minor, false);
}

void for_each_range_do(int (*do_it_function)(const char *major, const char *minor, bool mainline))
{
	struct version_range *vr;
	int ret;
	int x;

	// FIXME here is where we can thread the heck out of this.  Maybe...
	for (x = 0; x < max_version_range; x++) {
		vr = &version_range_array[x];
		ret = do_it_function(vr->v_from.name, vr->v_to.name, vr->mainline);
		if (ret) {
			printf("do_it failed for %s, %s, %d\n", vr->v_from.name, vr->v_to.name, vr->mainline);
			return;
		}

	}
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

static char *get_head_tag(void)
{
	git_describe_result *res;
	git_object *object;
	git_reference *ref;
	git_describe_format_options options;
	git_describe_options desc_options;
	const git_oid *oid;
	git_buf buf = { 0 };
	char *head_tag;
	int ret;

	// First, find the current release, which is the "newest" tag (i.e. description) of the head
	// that is in the master branch. "git describe --abbrev=0 master" does what we need.

	// Get the branch description of 'master'
	ret = git_branch_lookup(&ref, git_repo, "master", GIT_BRANCH_LOCAL);
	if (ret)
		fprintf(stderr, "git_branch_lookup() failed: %d\n", ret);

	oid = git_reference_target(ref);
	if (!oid)
		fprintf(stderr, "git_reference_target() failed\n");

	ret = git_object_lookup(&object, git_repo, oid, GIT_OBJECT_COMMIT);
	if (ret)
		fprintf(stderr, "git_object_lookup() failed: %d\n", ret);


	git_describe_options_init(&desc_options, GIT_DESCRIBE_OPTIONS_VERSION);
	desc_options.only_follow_first_parent = 1;

	ret = git_describe_commit(&res, object, &desc_options);
	if (ret)
		fprintf(stderr, "git_describe_commit() failed: %d\n", ret);

	git_describe_format_options_init(&options, GIT_DESCRIBE_FORMAT_OPTIONS_VERSION);
	options.abbreviated_size = 0;

	ret = git_describe_format(&buf, res, &options);
	if (ret)
		fprintf(stderr, "git_describe_format() failed: %d\n", ret);

	terminal_fprintf(stdout, "  git head tag = " TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT "\n",
			 buf.ptr);

	head_tag = strdup(buf.ptr);

	git_describe_result_free(res);
	git_object_free(object);
	git_reference_free(ref);
	git_buf_dispose(&buf);

	return head_tag;
}

static int loop_through_rc(void)
{
	char range1[256];
	char range2[256];
	char *head_tag;

	head_tag = get_head_tag();

	// If there is no "-rc" in the head, then nothing to do!
	char *rc = strstr(head_tag, "-rc");
	if (rc == NULL) {
		fprintf(stdout, "At a main release, no -rc release to generate.\n");
		goto exit;
	}

	// Let's start with the previous release and go to the current -rc1
	// Carve off the -rc from the string
	rc[0] = 0x00;

	// Find the first '.'
	char *dot = strstr(head_tag, ".");
	if (dot == NULL) {
		fprintf(stderr, "Error: can not parse version string '%s', exiting -rc attempt\n",
			head_tag);
		goto exit;
	}

	// Find the major and minor number of the release
	int major = atoi(&head_tag[1]);
	int minor = atoi(&dot[1]);

	snprintf(range1, sizeof(range1), "%d.%d", major, minor - 1);
	snprintf(range2, sizeof(range2), "%d.%d-rc1", major, minor);
	add_version_major(range2);

	// Let's walk through as many -rc releases as we can think of
	for (int i = 1; i < 12; ++i) {
		snprintf(range1, sizeof(range1), "v%s-rc%d", &head_tag[1], i);
		snprintf(range2, sizeof(range2), "v%s-rc%d", &head_tag[1], i + 1);
		if (!is_valid_release(range1) || !is_valid_release(range2))
			continue;

		snprintf(range1, sizeof(range1), "%s-rc%d", &head_tag[1], i);
		snprintf(range2, sizeof(range2), "%s-rc%d", &head_tag[1], i + 1);
		add_version_major(range2);
	}

exit:
	free(head_tag);
	return 0;
}

static void range_loop_through_2(void)
{
	char range1[256];
	char range2[256];
	char str[256];

	for (int x = 1; x < 41; ++x) {
		snprintf(str, sizeof(str), "v2.6.%d", x);
		if (!is_valid_release(str)) {
			dbg("%s is NOT a valid release\n", str);
			continue;
		}

		snprintf(range1, sizeof(range1), "2.6.%d", x - 1);
		snprintf(range2, sizeof(range2), "2.6.%d", x);
		add_version_range_major(range1, range2);

		for (int y = 1; y < 101; ++y) {
			snprintf(str, sizeof(str), "v2.6.%d.%d", x, y);
			if (!is_valid_release(str)) {
				dbg("%s is NOT a valid release\n", str);
				continue;
			}

			// If this is the first time through the loop, do it from the
			// major to the first minor release
			if (y == 1) {
				snprintf(range1, sizeof(range1), "2.6.%d", x);
				snprintf(range2, sizeof(range2), "2.6.%d.%d", x, y);
			} else {
				snprintf(range1, sizeof(range1), "2.6.%d.%d", x, y-1);
				snprintf(range2, sizeof(range2), "2.6.%d.%d", x, y);
			}
			add_version_range_minor(range1, range2);
		}
	}
}

static void range_loop_through_y(int major, int minor)
{
	char str[256];
	char range1[256];
	char range2[256];
	int y;

	for (y = 1; y < 400; ++y) {
		snprintf(str, sizeof(str), "v%d.%d.%d", major, minor, y);
		if (!is_valid_release(str)) {
			dbg("%s is NOT a valid release\n", str);
			continue;
		}

		if (y == 1) {
			// First time through the loop, do it from the major to
			// the first minor release
			snprintf(range1, sizeof(range1), "%d.%d", major, minor);
			snprintf(range2, sizeof(range2), "%d.%d.%d", major, minor, y);
		} else {
			snprintf(range1, sizeof(range1), "%d.%d.%d", major, minor, y-1);
			snprintf(range2, sizeof(range2), "%d.%d.%d", major, minor, y);
		}
		add_version_range_minor(range1, range2);
	}
}

static void range_loop_through_x(int major)
{
	char str[256];
	int minor;

	for (minor = 0; minor < 40; ++minor) {
		snprintf(str, sizeof(str), "v%d.%d", major, minor);
		if (!is_valid_release(str)) {
			dbg("%s is NOT a valid release\n", str);
			continue;
		}
		dbg("%s is a valid release\n", str);

		if (minor != 0) {
			char range1[256];
			char range2[256];
			snprintf(range1, sizeof(range1), "%d.%d", major, minor-1);
			snprintf(range2, sizeof(range2), "%d.%d", major, minor);
			add_version_range_major(range1, range2);
		}
		range_loop_through_y(major, minor);
	}

}

static int add_version_range_rc(void)
{
	char range1[256];
	char range2[256];
	char *head_tag;

	head_tag = get_head_tag();

	// If there is no "-rc" in the head, then nothing to do!
	char *rc = strstr(head_tag, "-rc");
	if (rc == NULL) {
		fprintf(stdout, "At a main release, no -rc release to generate.\n");
		goto exit;
	}

	// Let's start with the previous release and go to the current -rc1
	// Carve off the -rc from the string
	rc[0] = 0x00;

	// Find the first '.'
	char *dot = strstr(head_tag, ".");
	if (dot == NULL) {
		fprintf(stderr, "Error: can not parse version string '%s', exiting -rc attempt\n",
			head_tag);
		goto exit;
	}

	// Find the major and minor number of the release
	int major = atoi(&head_tag[1]);
	int minor = atoi(&dot[1]);

	snprintf(range1, sizeof(range1), "%d.%d", major, minor - 1);
	snprintf(range2, sizeof(range2), "%d.%d-rc1", major, minor);
	add_version_range_major(range1, range2);

	// Let's walk through as many -rc releases as we can think of
	for (int i = 1; i < 12; ++i) {
		snprintf(range1, sizeof(range1), "v%s-rc%d", &head_tag[1], i);
		snprintf(range2, sizeof(range2), "v%s-rc%d", &head_tag[1], i + 1);
		if (!is_valid_release(range1) || !is_valid_release(range2))
			continue;

		snprintf(range1, sizeof(range1), "%s-rc%d", &head_tag[1], i);
		snprintf(range2, sizeof(range2), "%s-rc%d", &head_tag[1], i + 1);
		add_version_range_major(range1, range2);
	}

exit:
	free(head_tag);
	return 0;
}


void versions_create(void)
{
	struct vh_timestamp *foo;
	double seconds;

	// We do all of this walking twice.
	//  - First to get all of the valid releases.
	//  - Second to set up the ranges between those releases (which is where the commits
	//    actually are)
	//
	// We could do this in one loop, BUT, the corner cases are tricky so to make it simpler, at
	// the expense of duplicated code, let's just do it twice.  Overall it's only about 1 second
	// to do all of this so saving .5 seconds, while really nice, isn't the largest time sink at
	// the moment, unfortunately.

	// Create the versions
	foo = time_start(__func__);
	loop_through_2();
	loop_through_x(3);
	loop_through_x(4);
	loop_through_x(5);
	loop_through_x(6);
	loop_through_rc();
	seconds = time_stop(foo);
	terminal_fprintf(stdout, "    Versions creation took "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", seconds);


	// Create the ranges
	foo = time_start(__func__);
	range_loop_through_2();
	range_loop_through_x(3);
	range_loop_through_x(4);
	range_loop_through_x(5);
	range_loop_through_x(6);

	// Do "special" releases where we jump a major number
	add_version_range_major("2.6.12-rc2", "2.6.12");
	add_version_range_major("2.6.39", "3.0");
	add_version_range_major("3.19", "4.0");
	add_version_range_major("4.20", "5.0");
	add_version_range_major("5.19", "6.0");

	// Fill in the last little bit of -rc release information if we have it in the tree
	add_version_range_rc();

	seconds = time_stop(foo);
	terminal_fprintf(stdout, "	"
			 TERMINAL_FG_CYAN "%d" TERMINAL_FG_DEFAULT
			 " version ranges created\n", max_version_range);
	terminal_fprintf(stdout, "    Version range creation took "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", seconds);
}
