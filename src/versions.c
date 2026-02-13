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
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <git2.h>
#include "verhaal.h"
#include "terminal.h"

#define NUM_VERSIONS	20000	// Good for a few more years...

/* Yes, we could use a vector, or linked list, but hey, this is userspace, we
 * have a ton of memory, just use a simple array and be done with it.
 */
static struct version version_array[NUM_VERSIONS];
static int max_version;
static int new_versions;

static struct version_range version_range_array[NUM_VERSIONS];
static int max_version_range;
int new_ranges;

// "Flag" to flip when we go from reading the information from the db to creating it from the git
// tree itself.  We use this to "know" if a version/range is new and we need to parse it from git
// and write that out to the disk
static bool new_version_flag;
static bool version_ranges_parallel;

struct range_worker_context {
	int (*do_it)(struct version_range *vr);
	int next_index;
	int error;
	const struct version_range *failed_vr;
};

bool version_ranges_parallel_active(void)
{
	return version_ranges_parallel;
}

static int determine_parallel_threads(void)
{
	int threads = 0;
	const char *env = getenv("VERHAAL_THREADS");

	if (env && *env) {
		char *endptr = NULL;
		errno = 0;
		long val = strtol(env, &endptr, 10);
		if (errno == 0 && endptr && *endptr == '\0' && val > 0 && val <= INT_MAX)
			threads = (int)val;
	}

	if (threads <= 0) {
		long nproc = sysconf(_SC_NPROCESSORS_ONLN) * 2;
		if (nproc > 0 && nproc <= INT_MAX)
			threads = (int)nproc;
	}

	if (threads <= 0)
		threads = 1;

	if (new_ranges > 0 && threads > new_ranges)
		threads = new_ranges;

	if (threads > max_version_range)
		threads = max_version_range;

	if (threads < 1)
		threads = 1;

	return threads;
}

static void *range_worker(void *data)
{
	struct range_worker_context *ctx = data;
	git_repository *local_repo = NULL;
	int ret;

	ret = git_repository_open(&local_repo, git_repository_path(git_repo));
	if (ret) {
		__sync_bool_compare_and_swap(&ctx->error, 0, ret);
		return NULL;
	}

	git_repo_set_thread(local_repo);

	for (;;) {
		if (__atomic_load_n(&ctx->error, __ATOMIC_RELAXED))
			break;

		int idx = __atomic_fetch_add(&ctx->next_index, 1, __ATOMIC_RELAXED);
		if (idx >= max_version_range)
			break;

		if (__atomic_load_n(&ctx->error, __ATOMIC_RELAXED))
			break;

		struct version_range *vr = &version_range_array[idx];
		ret = ctx->do_it(vr);
		if (ret) {
			int expected = 0;
			if (__atomic_compare_exchange_n(&ctx->error, &expected, ret, false,
							__ATOMIC_RELAXED, __ATOMIC_RELAXED))
				ctx->failed_vr = vr;
			break;
		}
	}

	git_repo_clear_thread();
	git_repository_free(local_repo);
	return NULL;
}

static bool is_valid_release(const char *version)
{
	int ret;
	git_object *obj = NULL;

	ret = git_revparse_single(&obj, git_repo_get(), version);
	if (ret)
		return false;
	git_object_free(obj);
	return true;
}

// Simple "is this version in our table or not
// Odds are it can be sped up, but really, it's a simple array read of memory, cpus do that fast
// these days...
static const struct version *find_version(const char *version)
{
	const struct version *v;
	int i;

	for (i = 0; i < max_version; ++i) {
		v = &version_array[i];
		if (!strcmp(version, v->name))
			return v;
	}
	return NULL;
}

void version_add(const char *version, bool mainline)
{
	struct version *v = &version_array[max_version];

	// Skip if we have seen this version already
	if (find_version(version))
		return;

	strcpy(v->name, version);
	v->mainline = mainline;
	v->new = new_version_flag;

	if (new_version_flag) {
		dbg("%s: new version %s	%d\n", __func__, version, mainline);
		new_versions++;
	}

	//printf("%d	%s	%d\n", max_version, version, mainline);
	max_version++;
	if (max_version > NUM_VERSIONS) {
		fprintf(stderr, "Number of versions just overflowed, fix NUM_VERSIONS to be bigger!\n");
		exit(1);
	}

	// Add the version to the database
	db_release_add(v);
}

static void add_version_major(const char *version)
{
	version_add(version, true);
}

static void add_version_minor(const char *version)
{
	version_add(version, false);
}

// Simple "is this version in our table or not
// Odds are it can be sped up, but really, it's a simple array read of memory, cpus do that fast
// these days...
static const struct version_range *find_version_range(const char *from, const char *to)
{
	const struct version_range *vr;
	int i;

	for (i = 0; i < max_version_range; ++i) {
		vr = &version_range_array[i];
		if ((!strcmp(from, vr->from.name)) && (!strcmp(to, vr->to.name)))
			return vr;
	}
	return NULL;
}

void version_range_add(const char *from, const char *to, bool mainline)
{
	struct version_range *vr = &version_range_array[max_version_range];

	// Skip if we have seen this version range already
	if (find_version_range(from, to))
		return;

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

	strcpy(vr->from.name, from);
	strcpy(vr->to.name, to);
	vr->mainline = mainline;
	vr->new = new_version_flag;
	list_head_init(&vr->commits);

	if (new_version_flag) {
		dbg("%s: new release %s	%s	%d\n", __func__, from, to, mainline);
		new_ranges++;
	}

	//printf("%s: from: %s	to: %s	mainline: %d\n", __func__, from, to, mainline);
	max_version_range++;

	db_range_add(vr);
}

static void add_version_range_major(const char *major, const char *minor)
{
	version_range_add(major, minor, true);
}

// Like add_version_range_major() but we verify these are valid version numbers
// before attempting to add them to the list
static void add_version_range_major_check(const char *from, const char *to)
{
	if (is_valid_release(from) &&
	    is_valid_release(to))
		add_version_range_major(from, to);
}

static void add_version_range_minor(const char *major, const char *minor)
{
	version_range_add(major, minor, false);
}

void for_each_range_do(int (*do_it_function)(struct version_range *vr))
{
	struct version_range *vr;
	int ret;
	int x;

	// FIXME here is where we can thread the heck out of this.  Maybe...
	for (x = 0; x < max_version_range; x++) {
		vr = &version_range_array[x];
		ret = do_it_function(vr);
		if (ret) {
			fprintf(stderr, "Error: %s range %s to %s failed.",
				vr->mainline ? "mainline" : "rc", vr->from.name, vr->to.name);
			return;
		}

	}
}

void for_each_range_do_parallel(int (*do_it_function)(struct version_range *vr))
{
	int threads;
	struct range_worker_context ctx = {
		.do_it = do_it_function,
		.next_index = 0,
		.error = 0,
		.failed_vr = NULL,
	};
	pthread_t *workers = NULL;
	int created = 0;

	threads = determine_parallel_threads();
	if (threads <= 1) {
		for_each_range_do(do_it_function);
		return;
	}

	fprintf(stdout, "  Using %d threads for range processing\n", threads);

	workers = calloc(threads, sizeof(*workers));
	if (!workers) {
		fprintf(stderr, "Out of memory, falling back to single-threaded range processing\n");
		for_each_range_do(do_it_function);
		return;
	}

	version_ranges_parallel = true;
	for (int i = 0; i < threads; ++i) {
		int ret = pthread_create(&workers[i], NULL, range_worker, &ctx);
		if (ret) {
			fprintf(stderr, "pthread_create failed (%d), falling back to single-threaded range processing\n",
				ret);
			ctx.error = 0;
			break;
		}
		created++;
	}

	if (created != threads) {
		for (int i = 0; i < created; ++i)
			pthread_join(workers[i], NULL);
		version_ranges_parallel = false;
		free(workers);
		for_each_range_do(do_it_function);
		return;
	}

	for (int i = 0; i < threads; ++i)
		pthread_join(workers[i], NULL);

	version_ranges_parallel = false;
	free(workers);

	if (ctx.error) {
		if (ctx.failed_vr)
			fprintf(stderr, "Error: %s range %s to %s failed.",
				ctx.failed_vr->mainline ? "mainline" : "rc",
				ctx.failed_vr->from.name,
				ctx.failed_vr->to.name);
		else
			fprintf(stderr, "Parallel range worker failed with error %d\n", ctx.error);
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
	static bool git_head_tag_print = false;
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
	ret = git_branch_lookup(&ref, git_repo_get(), "master", GIT_BRANCH_LOCAL);
	if (ret) {
		fprintf(stderr, "Fatal: Could not find 'master' branch (error %d). Please ensure your local repository has a 'master' branch.\n", ret);
		exit(1);
	}

	oid = git_reference_target(ref);
	if (!oid) {
		fprintf(stderr, "Fatal: Could not determine the target OID for the 'master' branch reference.\n");
		git_reference_free(ref);
		exit(1);
	}

	ret = git_object_lookup(&object, git_repo_get(), oid, GIT_OBJECT_COMMIT);
	if (ret) {
		fprintf(stderr, "Fatal: Could not lookup the commit object for the 'master' branch (error %d).\n", ret);
		git_reference_free(ref);
		exit(1);
	}


	git_describe_options_init(&desc_options, GIT_DESCRIBE_OPTIONS_VERSION);
	desc_options.only_follow_first_parent = 1;

	ret = git_describe_commit(&res, object, &desc_options);
	if (ret) {
		fprintf(stderr, "Fatal: 'git describe' failed for the master branch head (error %d).\n", ret);
		exit(1);
	}

	git_describe_format_options_init(&options, GIT_DESCRIBE_FORMAT_OPTIONS_VERSION);
	options.abbreviated_size = 0;

	ret = git_describe_format(&buf, res, &options);
	if (ret) {
		fprintf(stderr, "Fatal: Could not format the 'git describe' result (error %d).\n", ret);
		exit(1);
	}

	// Only print this out once.
	if (!git_head_tag_print) {
		terminal_fprintf(stdout, "    git head tag = " TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT "\n",
				 buf.ptr);
		git_head_tag_print = true;
	}

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
		dbg("At a main release, no -rc release to generate.\n");
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
		dbg("At a main release, no -rc release to generate.\n");
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

	// Set the flag to be true as we are now walking git
	new_version_flag = true;

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
	terminal_fprintf(stdout, "    "
			 TERMINAL_FG_CYAN "%d" TERMINAL_FG_DEFAULT
			 " versions total, "
			 TERMINAL_FG_CYAN "%d" TERMINAL_FG_DEFAULT
			 " versions are new, and everything handled in "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", max_version, new_versions, seconds);


	// Create the ranges
	foo = time_start(__func__);
	range_loop_through_2();
	range_loop_through_x(3);
	range_loop_through_x(4);
	range_loop_through_x(5);
	range_loop_through_x(6);
	range_loop_through_x(7);

	// Do "special" releases where we jump a major number
	add_version_range_major_check("2.6.12-rc2", "2.6.12");
	add_version_range_major_check("2.6.39", "3.0");
	add_version_range_major_check("3.19", "4.0");
	add_version_range_major_check("4.20", "5.0");
	add_version_range_major_check("5.19", "6.0");
	add_version_range_major_check("6.19", "7.0");

	// Fill in the last little bit of -rc release information if we have it in the tree
	add_version_range_rc();

	seconds = time_stop(foo);
	terminal_fprintf(stdout, "    "
			 TERMINAL_FG_CYAN "%d" TERMINAL_FG_DEFAULT
			 " version ranges total, "
			 TERMINAL_FG_CYAN "%d" TERMINAL_FG_DEFAULT
			 " ranges are new, and everything handled in "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", max_version_range, new_ranges, seconds);
}
