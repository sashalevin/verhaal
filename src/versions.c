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

static void add_version_range(const char *from, const char *to)
{
	struct version_range *vr = &version_range_array[max_version_range];

	strcpy(vr->v_from.name, from);
	strcpy(vr->v_to.name, to);

	// Don't care about the mainline value for now

	//printf("%d	%s	%d\n", max_version_range, from, to);
	max_version_range++;
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

static int loop_through_rc(void)
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
	char range1[256];
	char range2[256];

	// First, find the current release, which is the "newest" tag (i.e. description) of the head
	// that is in the master branch. "git describe --abbrev=0 master" does what we need.

#if 0
	// Cheat way, just call popen()!
	FILE *fp;
	char output[1024];
	fp = popen("cd /home/gregkh/linux/stable/linux-stable/ && git describe --abbrev=0", "r");
	fgets(output, 1024, fp);
	pclose(fp);
	fprintf(stdout, "result = %s", output);
#endif	// ok, let do it right...

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

void versions_create(void)
{
	struct vh_timestamp *foo;
	double seconds;

	foo = time_start(__func__);
	loop_through_2();
	loop_through_x(3);
	loop_through_x(4);
	loop_through_x(5);
	loop_through_x(6);
	loop_through_rc();
	seconds = time_stop(foo);
	terminal_fprintf(stdout, "    Versions create took "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", seconds);
}
