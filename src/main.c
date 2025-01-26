// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
// 'verhaal' - builds a database of kernel commits so that we can search them later on
//
// This is a "replacement" for the old-school "abuse the filesystem as a database" tool that we use
// to store all Linux kernel mainline and stable kernel commits in so that we can "quickly" search
// them.
//
// Instead of using the database and grep we do it all in a sql database and then we can search it
// with some simple sql statements.  Cuts time to search from about .6 seconds to .01 seconds on my
// semi-slow-storage device.
//
// This is the "create the database" program.  It does it all in ram and then writes it out to disk
// when finished as that's much faster.  It recreates the world, all at once.  Ideally we should
// "just" add the new records when they are found, but that's for another day...
//

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <sqlite3.h>
#include <git2.h>
#include "ccan/list/list.h"

#include "verhaal.h"
#include "terminal.h"

struct commit {
	struct list_head node;
	char *id;
	char *release;
	char *mainline_id;
	char *reverts;
	char *fixes;
};

static git_repository *git_repo;
static char *git_repo_location;
static char *database_name;
static const char *database_name_default = DATABASE_NAME;

static bool fixes_print = false;

// We have a PRIMARY KEY although it is probably not needed because git ensures us of this anyway...
static const char *db_create_commits_sql =	"CREATE TABLE IF NOT EXISTS commits "	\
						"(id TEXT PRIMARY KEY NOT NULL, "	\
						" release TEXT NOT NULL, "		\
						" mainline INTEGER,"			\
						" mainline_id TEXT,"			\
						" reverts TEXT,"			\
						" fixes TEXT);";

static const char *db_create_releases_sql =	"CREATE TABLE IF NOT EXISTS releases "	\
						"(release TEXT PRIMARY KEY NOT NULL, "	\
						"mainline INTEGER);";

static struct sqlite3 *database;

// Dumb "only print stuff when greg is debugging the code" function
static bool debug = false;
__attribute__((__format__(printf, 1, 2))) static int dbg(const char *fmt, ...)
{
	va_list args;
	int ret;

	if (!debug)
		return 0;

	va_start(args, fmt);
	ret = vprintf(fmt, args);
	va_end(args);

	return ret;
}

static int db_init(void)
{
	char *error;
	int ret;

	// We open an in-memory database to create everything, and then write it all out at the very
	// end.  Hack, yes, but fast, blazingly.
	ret = sqlite3_open(":memory:", &database);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error opening in-memory database %s\n",
			sqlite3_errmsg(database));
		sqlite3_close(database);
		return ret;
	}

	/* Create the tables if it's not been initialized yet */
	ret = sqlite3_exec(database, db_create_commits_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error creating database %s %s\n",
			database_name, error);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	// Stick in the "first" commit as we have to do it by hand for some reason (git doesn't like
	// showing it for us...)
	const char *db_initial_commit_sql = "INSERT INTO commits (id, release, mainline) VALUES ('1da177e4c3f41524e886b7f1b8a0c1fc7321cac2', '2.6.12', 1);";
	ret = sqlite3_exec(database, db_initial_commit_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error adding initial commit in database %s %s\n",
			database_name, error);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	sqlite3_free(error);
	return ret;
}

static int db_releases_init(void)
{
	char *error;
	int ret;

	/* Create the releases table */
	ret = sqlite3_exec(database, db_create_releases_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error creating database %s %s\n",
			database_name, error);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	// Stick in the "first" commit as we have to do it by hand for some reason (git doesn't like
	// showing it for us...)
	const char *db_initial_release_sql = "INSERT INTO releases (release, mainline) VALUES ('2.6.12', 1);";
	ret = sqlite3_exec(database, db_initial_release_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error adding initial commit in database %s %s\n",
			database_name, error);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	sqlite3_free(error);
	return ret;
}

static const char *db_insert_release_sql = "INSERT INTO releases (release, mainline) VALUES (?, ?);";
static int db_release_add(const char *release, int mainline)
{
	sqlite3_stmt *sql_stmt = NULL;
	int ret;

	dbg("%s: %10s mainline=%d\n", __func__, release, mainline);

	ret = sqlite3_prepare(database, db_insert_release_sql, -1, &sql_stmt, NULL);
	if (ret) {
		fprintf(stderr, "Error preparing release sql statement %s\n",
			sqlite3_errmsg(database));
		return ret;
	}
	sqlite3_bind_text(sql_stmt, 1, release, strlen(release), NULL);
	sqlite3_bind_int(sql_stmt, 2, mainline);

	ret = sqlite3_step(sql_stmt);
	if (ret != SQLITE_DONE)
		fprintf(stderr, "Error inserting release %s row %s\n", release, sqlite3_errmsg(database));

	ret = sqlite3_finalize(sql_stmt);

	return ret;
}

static void db_shutdown(void)
{
	sqlite3_close(database);
}

// Write the in-memory database out to disk
static int db_write_to_disk(void)
{
	int ret;
	sqlite3 *file;
	sqlite3_backup *backup;

	terminal_fprintf(stdout, "  Writing to database file '"
			 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT "'\n", database_name);

	ret = sqlite3_open(database_name, &file);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error, unable to open database file %s\n", database_name);
		return ret;
	}

	backup = sqlite3_backup_init(file, "main", database, "main");
	if (backup) {
		sqlite3_backup_step(backup, -1);
		sqlite3_backup_finish(backup);
	}
	ret = sqlite3_errcode(file);

	sqlite3_close(file);
	return ret;
}

/*
 * So, it turns out it is FASTER to just write out the whole git history at
 * once, instead of attempting to read it from disk, then query the database
 * for every minor kernel release to see if it is already in the database
 * with something like a sql statement of:
 *	SELECT COUNT() FROM commits WHERE release='6.1';
 * This function has the logic to read the database from the disk, but really,
 * don't call it, it's just here to show the attempt.
 */
#if 0
static int db_read_from_disk(void)
{
	int ret;
	sqlite3 *file;
	sqlite3_backup *backup;
	char *error;

	fprintf(stdout, "Reading from database %s\n", database_name);

	// Open the on-disk database, if present
	ret = sqlite3_open(database_name, &file);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error, unable to open database file %s\n", database_name);
		return ret;
	}
	// See if it really is our database, or if it's just an empty file
	// (which sqlite3_open() will create if not present already)
	const char *db_test_sql = "SELECT * from commits;";
	ret = sqlite3_exec(file, db_test_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		// Query did not work, so commits table is not there, let's
		// abort and initialize this later on
		fprintf(stderr, "on-disk database '%s' is not present, will initialize it now.\n",
			database_name);
		sqlite3_free(error);
		sqlite3_close(file);
		return ret;
	}

	// Open in-memory database to read into
	ret = sqlite3_open(":memory:", &database);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error opening in-memory database %s\n", sqlite3_errmsg(database));
		sqlite3_close(database);
		return ret;
	}

	backup = sqlite3_backup_init(database, "main", file, "main");
	if (backup) {
		sqlite3_backup_step(backup, -1);
		sqlite3_backup_finish(backup);
	}
	ret = sqlite3_errcode(file);
	if (ret)
		fprintf(stderr, "Error reading from database into memory: %d\n", ret);

	sqlite3_close(file);
	return ret;
}
#endif

static int git_init(void)
{
	int ret;

	git_libgit2_init();

	ret = git_repository_open(&git_repo, git_repo_location);
	if (ret) {
		fprintf(stderr, "Error opening git repo at %s", git_repo_location);
		return ret;
	}

	return 0;
}

static void git_shutdown(void)
{
	git_repository_free(git_repo);
	git_libgit2_shutdown();
}

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

static char *find_sha1_full(const char *string)
{
	return search_string(string, "[a-f0-9]{40,}");
}

static char *find_sha1_short(const char *string)
{
	// At least 10 characters long, we might miss some odd ones, but
	// it's a good start as they _should_ all be at least 12 long.
	return search_string(string, "[a-f0-9]{10,}");
}

static char *find_upstream(const char *message)
{
	char *upstream;
	char *sha1;

	upstream = search_string(message, ".*upstream.*\n?");
	if (!upstream)
		return NULL;

	sha1 = find_sha1_full(upstream);
	free(upstream);
	return sha1;
}

static char *find_reverts(const char *message)
{
	char *reverts;
	char *sha1;

	reverts = search_string(message, ".*reverts.*\n?");
	if (!reverts)
		return NULL;
	sha1 = find_sha1_full(reverts);
	free(reverts);
	return sha1;
}

// Handle a single "Fixes:" line
static char *find_fix(const char *line)
{
	char *fix;
	char *sha1;

	fix = search_string(line, "fixes:.*\n?");
	if (!fix)
		return NULL;
	sha1 = find_sha1_short(fix);
	free(fix);
	return sha1;
}

// Turn a "short" Fixes SHA1 into a "full" SHA1 if it is present in the git tree
static char *fixes_expand(char *fix, const char *line)
{
	git_revspec revspec;
	const git_oid *oid;
	char *sha;

	int ret = git_revparse(&revspec, git_repo, fix);

	if (ret) {
		// fix was not present!
		// FIXME for now just return the string given to us, we'll fix this later...
		//fprintf(stderr, "Invalid fix line: '%s' '%s'", fix, line);
		//fprintf(stderr, "%s is NOT a valid fix in the kernel tree, please fix...\n", fix);

		if (fixes_print) {
			// Dig out the SHA1: ("foo") type info so we can look them up elsewhere
			char *temp = search_string(line, "[a-f0-9]{10,}.*");
			fprintf(stderr, "%s\n", temp);
			free(temp);
		}
		return fix;
	}

	// Turn the git oid into a full sha1
	oid = git_object_id(revspec.from);
	sha = malloc(100);
	git_oid_tostr(sha, 100, oid);
	dbg("		short fixes: %s, expanded sha = %s\n", fix, sha);

	git_object_free(revspec.from);
	git_object_free(revspec.to);
	free(fix);
	return sha;
}

#if 0
// Test message to check for multiple Fixes: lines
static const char *test_message =
"    Reported-by: Juri Lelli <juri.lelli@redhat.com> # original bug\n"
"    Reported-by: Manu Bretelle <chantra@meta.com> # bugs in masking fix\n"
"    Fixes: 3f00c5239344 (\"bpf: Allow trusted pointers to be passed to KF_TRUSTED_ARGS kfuncs\")\n"
"    Fixes: cb4158ce8ec8 (\"bpf: Mark raw_tp arguments with PTR_MAYBE_NULL\")\n"
"    Reviewed-by: Eduard Zingerman <eddyz87@gmail.com>\n"
"    Co-developed-by: Jiri Olsa <jolsa@kernel.org>";
#endif

// Handle the message one line at a time, as that's simpler than attempting a recursive search of a
// large buffer
static char *find_fixes(const char *message)
{
	char *final, *temp;

	final = malloc(1024);
	memset(final, 0x00, 1024);
	temp = malloc(1024);

	// Split the message up into one-per-line in a destructive way
	// and then feed that to find_fix and build up a string of sha1
	// values as a "fixes" line
	char *local_message = strdup(message);
	char *line = strtok(local_message, "\n");
	while (line) {
		//fprintf(stdout, "line: '%s'\n", line);
		char *f = find_fix(line);
		if (f) {
			// turn this into a fully-formed SHA1, not just an abbreviated one
			f = fixes_expand(f, line);
			if (strlen(final) == 0)
				snprintf(temp, 1024, "%s", f);
			else
				snprintf(temp, 1024, "%s %s", final, f);

			strcpy(final, temp);
			free(f);
		}
		line = strtok(NULL, "\n");
	}
	free(local_message);
	//fprintf(stdout, "final='%s'\n", final);
	free(temp);

	// If we didn't find anything, just return NULL to keep things sane
	if (strlen(final) == 0) {
		free(final);
		return NULL;
	}
	return final;
}

static const char *db_insert_sql = "INSERT INTO commits (id, release, mainline, mainline_id, reverts, fixes) VALUES (?, ?, ?, ?, ?, ?);";

static int create_kernel_range(const char *start, const char *end, bool minor)
{
	char *upstream = NULL;
	char *reverts = NULL;
	char *fixes = NULL;
	char range[256];
	git_oid oid;
	git_revwalk *walker;
	int ret;
	int mainline;

	// Set "is this mainline or not" flag to be stored later
	if (minor)
		mainline = 0;
	else
		mainline = 1;

	dbg("%s: start=%s, end=%s, mainline=%d\n", __func__, start, end, mainline);

	// Let's first see if these are a few "known" ranges that we know we can
	// never find, thanks to the start of the git repo and how the first few
	// tags were set up.
	// (i.e. v2.6.11 is NOT a real commit, but rather a tree.)
	if ((strcmp(start, "2.6.11") == 0) || (strcmp(end, "2.6.11") == 0)) {
		dbg("skipping invalid 2.6.11 commit as that's just a mess\n");
		return 0;
	}
	if ((strcmp(end, "3.16.35") == 0) || (strcmp(end, "2.6.11") == 0)) {
		dbg("skipping invalid 3.16.35 commit as there was a 'break' there\n");
		return 0;
	}

	// Loop through all git ids in this range, take the id and version and store it in the
	// database
	ret = git_revwalk_new(&walker, git_repo);
	if (ret) {
		fprintf(stderr, "Error, can not init a revwalk object\n");
		return ret;
	}

	snprintf(range, sizeof(range), "v%s..v%s", start, end);

	ret = git_revwalk_push_range(walker, range);
	if (ret) {
		fprintf(stderr, "Error, can not get the range of '%s'\n", range);
		git_revwalk_free(walker);
		return ret;
	}

	terminal_fprintf(stdout, TERMINAL_SAVE_CURSOR);
	terminal_fprintf(stdout, "  Processing kernel commits from "
			 TERMINAL_FG_BLUE "v%s" TERMINAL_FG_DEFAULT " to "
			 TERMINAL_FG_BLUE "v%s" TERMINAL_FG_DEFAULT "" TERMINAL_CLEAR_RIGHT, start, end);
	fflush(stdout);

	db_release_add(end, mainline);

	while (!git_revwalk_next(&oid, walker)) {
		char sha[256];
		sqlite3_stmt *sql_stmt = NULL;
		const char *message;
		git_commit *commit;

		// Turn the git oid into a full sha1
		git_oid_tostr(sha, sizeof(sha), &oid);

		// Get the git commit message so we can search it for stuff
		ret = git_commit_lookup(&commit, git_repo, &oid);
		if (ret) {
			fprintf(stderr, "git message lookup for %s failed\n", sha);
			continue;
		}

		message = git_commit_message(commit);

		// If this is a minor range, search the changelog message to figure out if this is
		// an upstream id, and if so, what it is and then save it off.
		if (minor) {
			upstream = find_upstream(message);
			if (upstream)
				dbg("	upstream=%s\n", upstream);
		}

		// Find if this is a revert
		reverts = find_reverts(message);
		if (reverts)
			dbg("	reverts=%s\n", reverts);

		// Find if this commit fixes anything
		fixes = find_fixes(message);
		if (fixes)
			dbg("	fixes=%s\n", fixes);

		git_commit_free(commit);

		// Save it in the database
		ret = sqlite3_prepare(database, db_insert_sql, -1, &sql_stmt, NULL);
		if (ret) {
			fprintf(stderr, "Error preparing sql statement %s\n",
				sqlite3_errmsg(database));
			goto exit;
		}
		sqlite3_bind_text(sql_stmt, 1, sha, strlen(sha), NULL);
		sqlite3_bind_text(sql_stmt, 2, end, strlen(end), NULL);
		sqlite3_bind_int(sql_stmt, 3, mainline);

		if (upstream)
			sqlite3_bind_text(sql_stmt, 4, upstream, strlen(upstream), NULL);

		if (reverts)
			sqlite3_bind_text(sql_stmt, 5, reverts, strlen(reverts), NULL);

		if (fixes)
			sqlite3_bind_text(sql_stmt, 6, fixes, strlen(fixes), NULL);

		ret = sqlite3_step(sql_stmt);
		if (ret != SQLITE_DONE)
			fprintf(stderr, "Error inserting commit %s row %s\n", sha, sqlite3_errmsg(database));

		ret = sqlite3_finalize(sql_stmt);
		if (upstream)
			free(upstream);
		if (reverts)
			free(reverts);
		if (fixes)
			free(fixes);
	}
	ret = 0;

exit:
	git_revwalk_free(walker);
	terminal_fprintf(stdout, TERMINAL_RESTORE_CURSOR);
	fflush(stdout);
	return ret;
}

static int create_kernel_range_major(const char *major, const char *minor)
{
	return create_kernel_range(major, minor, false);
}

static int create_kernel_range_minor(const char *major, const char *minor)
{
	return create_kernel_range(major, minor, true);
}

static int create_kernel_range_rc(void)
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
	create_kernel_range_major(range1, range2);

	// Let's walk through as many -rc releases as we can think of
	for (int i = 1; i < 12; ++i) {
		snprintf(range1, sizeof(range1), "v%s-rc%d", &head_tag[1], i);
		snprintf(range2, sizeof(range2), "v%s-rc%d", &head_tag[1], i + 1);
		if (!is_valid_release(range1) || !is_valid_release(range2))
			continue;

		snprintf(range1, sizeof(range1), "%s-rc%d", &head_tag[1], i);
		snprintf(range2, sizeof(range2), "%s-rc%d", &head_tag[1], i + 1);
		create_kernel_range_major(range1, range2);
	}

exit:
	free(head_tag);
	return 0;
}

static void loop_through_y(int major, int minor)
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
		create_kernel_range_minor(range1, range2);
	}
}

static void loop_through_x(int major)
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
			create_kernel_range_major(range1, range2);
		}
		loop_through_y(major, minor);
	}

}

static void loop_through_2(void)
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
		create_kernel_range_major(range1, range2);

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
			create_kernel_range_minor(range1, range2);
		}
	}
}

static const char *short_options = "Vvhfd:";

static const struct option long_options[] = {
	{
		.val =		'V',
		.name =		"version",
		.has_arg =	no_argument,
		.flag =		NULL,
	},
	{
		.val =		'v',
		.name =		"verbose",
		.has_arg =	no_argument,
		.flag =		NULL,
	},
	{
		.val =		'h',
		.name =		"help",
		.has_arg =	no_argument,
		.flag =		NULL,
	},
	{
		.val =		'f',
		.name =		"fixes",
		.has_arg =	no_argument,
		.flag =		NULL,
	},
	{
		.val =		'd',
		.name =		"database",
		.has_arg =	required_argument,
		.flag =		NULL,
	},
};

static void help(void)
{
	fprintf(stdout, "  Creates a database file of all of the Linux stable branch commits.\n");
	fprintf(stdout, "  This is used by the vulns.git CVE scripts, and other tools to track\n");
	fprintf(stdout, "  commits as they are made across multiple branches.\n\n");
	fprintf(stdout, "  Valid options:\n");
	fprintf(stdout, "	--help  -h	This message\n");
	fprintf(stdout, "	--fixes -f	Any invalid \"Fixes:\" lines will get written to stderr\n");
	fprintf(stdout, "	--verbose -V	Turn debugging messages on (warning, lots of junk here)\n");
	fprintf(stdout, "	--version -v	Print the version of the program and exit\n");
	fprintf(stdout, "	--database=	Change the default database name from '%s' to the provided one\n",
		database_name);
	exit(1);
}

static int get_options(int argc, char *argv[])
{
	const char *env_string;
	int option;

	env_string = getenv("CVEKERNELTREE");
	if (!env_string) {
		fprintf(stderr, "Error: Environment variable 'CVEKERNELTREE' must be set to point to\n");
		fprintf(stderr, "       the Linux kernel stable git repository directory.\n");
		return -1;
	}
	git_repo_location = strdup(env_string);

	while ((option = getopt_long(argc, argv, short_options, long_options, NULL)) != EOF) {
		switch (option) {
		case 'V':
			debug = true;
			break;

		case 'v':
			exit(0);

		case 'f':
			fixes_print = true;
			break;

		case 'd':
			database_name = strdup(optarg);
			break;

		case 'h':
		default:
			help();
		}
	}

	if (database_name == NULL)
		database_name = strdup(database_name_default);

	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	terminal_fprintf(stdout, TERMINAL_FG_GREEN "%s" TERMINAL_FG_DEFAULT
			 " version " TERMINAL_FG_BLUE "%s" TERMINAL_FG_DEFAULT "\n",
			 PACKAGE_NAME, VERSION);

	ret = get_options(argc, argv);
	if (ret)
		return ret;

	terminal_fprintf(stdout, "  Reading from stable kernel tree at '"
			 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT "'\n", git_repo_location);
	terminal_fprintf(stdout, "  Database file is '"
			 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT "'\n", database_name);

	ret = db_init();
	if (ret)
		goto exit;
	ret = db_releases_init();
	if (ret)
		goto exit;

	ret = git_init();
	if (ret)
		goto exit;

	loop_through_2();
	loop_through_x(3);
	loop_through_x(4);
	loop_through_x(5);
	loop_through_x(6);

	// Do "special" releases where we jump a major number
	create_kernel_range_major("2.6.12-rc2", "2.6.12");
	create_kernel_range_major("2.6.39", "3.0");
	create_kernel_range_major("3.19", "4.0");
	create_kernel_range_major("4.20", "5.0");
	create_kernel_range_major("5.19", "6.0");

	terminal_fprintf(stdout, "\n");

	// Fill in the last little bit of -rc release information if we have it in the tree
	create_kernel_range_rc();

	terminal_fprintf(stdout, "\n");

	struct vh_timestamp *foo = time_start("git_shutdown");
	git_shutdown();
	time_stop(foo);

	foo = time_start("Write data to disk");
	db_write_to_disk();
	time_stop(foo);

exit:
	free(database_name);
	free(git_repo_location);
	db_shutdown();
	return ret;
}
