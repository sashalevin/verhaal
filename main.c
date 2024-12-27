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
#include <sqlite3.h>
#include <git2.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>	// Now we have 2 problems...

static git_repository *git_repo;

static char *git_repo_location;

static const char *database_name = "commits.db";

// We have a PRIMARY KEY although it is probably not needed because git ensures us of this anyway...
static const char *db_create_sql =	"CREATE TABLE IF NOT EXISTS commits "	\
					"(id TEXT PRIMARY KEY NOT NULL, "	\
					" release TEXT NOT NULL, "		\
					" mainline_id TEXT,"			\
					" reverts TEXT,"			\
					" fixes TEXT);";

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
	ret = sqlite3_exec(database, db_create_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error creating database %s %s\n",
			database_name, error);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	// Stick in the "first" commit as we have to do it by hand for some reason (git doesn't like
	// showing it for us...)
	const char *db_initial_commit_sql = "INSERT INTO commits (id, release) VALUES ('1da177e4c3f41524e886b7f1b8a0c1fc7321cac2', '2.6.12');";
	ret = sqlite3_exec(database, db_initial_commit_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error adding initial commit in database %s %s\n",
			database_name, error);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

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

	fprintf(stdout, "Writing database to %s\n", database_name);

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

static int db_read_from_disk(void)
{
	int ret;
	sqlite3 *file;
	sqlite3_backup *backup;
	char *error;

	// Not working yet...
	return -1;

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

static char *find_upstream(const char *message)
{
	int ret;
	char *upstream = NULL;
	pcre2_code *re_upstream;
	pcre2_code *re_commit_id;
	int errornumber;
	pcre2_match_data *match_upstream;
	pcre2_match_data *match_commit;
	PCRE2_SIZE erroroffset;
	PCRE2_SPTR upstream_pattern = (PCRE2_SPTR8)".*upstream.*\n?";
	PCRE2_SPTR sha_pattern = (PCRE2_SPTR8)"[a-f0-9]{40,}";

	// We want to search all lines, find one with "upstream" on it, and then
	// find the sha1 in that line.  Takes two passes, if I was really good, I
	// could do it all in one regular expression.  As it is, it takes me two...
	// Here it is in bash:
	// mainlinesha=$(grep -i upstream ${message}| grep -oE "[a-f0-9]{40,}")

	// initialize our regular expression to find the upstream commit id
	re_upstream = pcre2_compile(upstream_pattern, PCRE2_ZERO_TERMINATED,
				    PCRE2_CASELESS, &errornumber, &erroroffset, NULL);
	if (!re_upstream) {
		fprintf(stderr, "pcre regex for upstream is not created.\n");
		goto exit;
	}
	match_upstream = pcre2_match_data_create_from_pattern(re_upstream, NULL);

	re_commit_id = pcre2_compile(sha_pattern, PCRE2_ZERO_TERMINATED,
				     PCRE2_CASELESS, &errornumber, &erroroffset, NULL);
	if (!re_commit_id) {
		fprintf(stderr, "pcre regex for sha pattern is not created.\n");
		goto exit;
	}
	match_commit = pcre2_match_data_create_from_pattern(re_commit_id, NULL);

	ret = pcre2_match(re_upstream, (PCRE2_SPTR8)message, strlen(message), 0, 0, match_upstream, NULL);
	if (ret > 0) {
		// match worked!
		PCRE2_SIZE *ovector;

		ovector = pcre2_get_ovector_pointer(match_upstream);
		for (int i = 0; i < ret; ++i) {
			PCRE2_SPTR substring_start = (PCRE2_SPTR8)message + ovector[2*i];
			size_t substring_length = ovector[2*i+1] - ovector[2*i];
			//printf("	%2d: %.*s\n", i, (int)substring_length, (char *)substring_start);

			// Now do the second search of the line for the sha
			int ret2 = pcre2_match(re_commit_id, substring_start, substring_length, 0, 0, match_commit, NULL);
			if (ret2 > 0) {
				// match found something!
				PCRE2_SIZE *ovector2;

				ovector2 = pcre2_get_ovector_pointer(match_commit);
				for (int j = 0; j < ret2; ++j) {
					PCRE2_SPTR substring_start2 = (PCRE2_SPTR8)substring_start + ovector2[2*i];
					size_t substring_length2 = ovector2[2*i+1] - ovector2[2*i];
					//printf("	%2d: %.*s\n", i, (int)substring_length2, (char *)substring_start2);
					upstream = malloc(substring_length2 + 1);
					memcpy(upstream, substring_start2, substring_length2);
					upstream[substring_length2] = 0x00;
				}
			}
		}
	}

	pcre2_match_data_free(match_upstream);
	pcre2_code_free(re_commit_id);
	pcre2_code_free(re_upstream);
exit:
	return upstream;
}

// Copy of find_mainline up above, make this better someday...
static char *find_reverts(const char *message)
{
	int ret;
	char *upstream = NULL;
	pcre2_code *re_upstream;
	pcre2_code *re_commit_id;
	int errornumber;
	pcre2_match_data *match_upstream;
	pcre2_match_data *match_commit;
	PCRE2_SIZE erroroffset;
	PCRE2_SPTR upstream_pattern = (PCRE2_SPTR8)".*reverts.*\n?";
	PCRE2_SPTR sha_pattern = (PCRE2_SPTR8)"[a-f0-9]{40,}";

	// initialize our regular expression to find the upstream commit id
	re_upstream = pcre2_compile(upstream_pattern, PCRE2_ZERO_TERMINATED,
				    PCRE2_CASELESS, &errornumber, &erroroffset, NULL);
	if (!re_upstream) {
		fprintf(stderr, "pcre regex for upstream is not created.\n");
		goto exit;
	}
	match_upstream = pcre2_match_data_create_from_pattern(re_upstream, NULL);

	re_commit_id = pcre2_compile(sha_pattern, PCRE2_ZERO_TERMINATED,
				     PCRE2_CASELESS, &errornumber, &erroroffset, NULL);
	if (!re_commit_id) {
		fprintf(stderr, "pcre regex for sha pattern is not created.\n");
		goto exit;
	}
	match_commit = pcre2_match_data_create_from_pattern(re_commit_id, NULL);

	ret = pcre2_match(re_upstream, (PCRE2_SPTR8)message, strlen(message), 0, 0, match_upstream, NULL);
	if (ret > 0) {
		// match worked!
		PCRE2_SIZE *ovector;

		ovector = pcre2_get_ovector_pointer(match_upstream);
		for (int i = 0; i < ret; ++i) {
			PCRE2_SPTR substring_start = (PCRE2_SPTR8)message + ovector[2*i];
			size_t substring_length = ovector[2*i+1] - ovector[2*i];
			//printf("	%2d: %.*s\n", i, (int)substring_length, (char *)substring_start);

			// Now do the second search of the line for the sha
			int ret2 = pcre2_match(re_commit_id, substring_start, substring_length, 0, 0, match_commit, NULL);
			if (ret2 > 0) {
				// match found something!
				PCRE2_SIZE *ovector2;

				ovector2 = pcre2_get_ovector_pointer(match_commit);
				for (int j = 0; j < ret2; ++j) {
					PCRE2_SPTR substring_start2 = (PCRE2_SPTR8)substring_start + ovector2[2*i];
					size_t substring_length2 = ovector2[2*i+1] - ovector2[2*i];
					//printf("	%2d: %.*s\n", i, (int)substring_length2, (char *)substring_start2);
					upstream = malloc(substring_length2 + 1);
					memcpy(upstream, substring_start2, substring_length2);
					upstream[substring_length2] = 0x00;
				}
			}
		}
	}

	pcre2_match_data_free(match_upstream);
	pcre2_match_data_free(match_commit);
	pcre2_code_free(re_commit_id);
	pcre2_code_free(re_upstream);
exit:
	return upstream;
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

// TODO : disambiguate the fixes sha1 values so that they are the "full" commit id to make searching
// "more correct" later on.  https://lore.kernel.org/r/20241218233613.219345-1-sashal@kernel.org has
// a bash script for this type of thing as an example


// Handle a single "Fixes:" line
static char *find_fix(const char *line)
{
	int ret;
	char *upstream = NULL;
	pcre2_code *re_fixes;
	pcre2_code *re_commit_id;
	int errornumber;
	pcre2_match_data *match_fixes;
	pcre2_match_data *match_commit;
	PCRE2_SIZE erroroffset;
	PCRE2_SPTR fixes_pattern = (PCRE2_SPTR8)".*fixes:.*\n?";
	//PCRE2_SPTR sha_pattern = (PCRE2_SPTR8)"[a-f0-9]{40,}";
	PCRE2_SPTR sha_pattern = (PCRE2_SPTR8)"[a-f0-9]{10,}";	// At least 10 characters long, we
								// might miss some odd ones, but
								// it's a good start as they
								// _should_ all be at least 12 long

	// initialize our regular expression to find the "Fixes:" line
	re_fixes = pcre2_compile(fixes_pattern, PCRE2_ZERO_TERMINATED,
				    PCRE2_CASELESS, &errornumber, &erroroffset, NULL);
	if (!re_fixes) {
		fprintf(stderr, "pcre regex for upstream is not created.\n");
		goto exit;
	}
	match_fixes = pcre2_match_data_create_from_pattern(re_fixes, NULL);

	// initialize our regular expression for the SHA1 line
	re_commit_id = pcre2_compile(sha_pattern, PCRE2_ZERO_TERMINATED,
				     PCRE2_CASELESS, &errornumber, &erroroffset, NULL);
	if (!re_commit_id) {
		fprintf(stderr, "pcre regex for sha pattern is not created.\n");
		goto exit;
	}
	match_commit = pcre2_match_data_create_from_pattern(re_commit_id, NULL);

	ret = pcre2_match(re_fixes, (PCRE2_SPTR8)line, strlen(line), 0, 0, match_fixes, NULL);
	if (ret > 0) {
		// match worked!
		PCRE2_SIZE *ovector;

		ovector = pcre2_get_ovector_pointer(match_fixes);
		for (int i = 0; i < ret; ++i) {
			PCRE2_SPTR substring_start = (PCRE2_SPTR8)line + ovector[2*i];
			size_t substring_length = ovector[2*i+1] - ovector[2*i];
			//printf("	%2d: %.*s\n", i, (int)substring_length, (char *)substring_start);

			// Now do the second search of the line for the sha
			int ret2 = pcre2_match(re_commit_id, substring_start, substring_length, 0, 0, match_commit, NULL);
			if (ret2 > 0) {
				// match found something!
				PCRE2_SIZE *ovector2;

				ovector2 = pcre2_get_ovector_pointer(match_commit);
				for (int j = 0; j < ret2; ++j) {
					PCRE2_SPTR substring_start2 = (PCRE2_SPTR8)substring_start + ovector2[2*i];
					size_t substring_length2 = ovector2[2*i+1] - ovector2[2*i];
					//printf("	%2d: %.*s\n", i, (int)substring_length2, (char *)substring_start2);
					upstream = malloc(substring_length2 + 1);
					memcpy(upstream, substring_start2, substring_length2);
					upstream[substring_length2] = 0x00;
				}
			}
		}
	}

	pcre2_match_data_free(match_fixes);
	pcre2_match_data_free(match_commit);
	pcre2_code_free(re_commit_id);
	pcre2_code_free(re_fixes);
exit:
	return upstream;
}

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
			// FIXME: turn this into a fully-formed SHA1, not just an abbreviated one
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

static const char *db_insert_sql = "INSERT INTO commits (id, release, mainline_id, reverts, fixes) VALUES (?, ?, ?, ?, ?);";

static int create_kernel_range(const char *start, const char *end, bool minor)
{
	char *upstream = NULL;
	char *reverts = NULL;
	char *fixes = NULL;
	char range[256];
	git_oid oid;
	git_revwalk *walker;
	int ret;

	dbg("%s: start=%s, end=%s, minor=%d\n", __func__, start, end, minor);

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

	fprintf(stdout, "Parsing kernel commits from v%s to v%s\n", start, end);
	fflush(stdout);

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

		// Save it in the databse
		ret = sqlite3_prepare(database, db_insert_sql, -1, &sql_stmt, NULL);
		if (ret) {
			fprintf(stderr, "Error preparing sql statement %s\n",
				sqlite3_errmsg(database));
			goto exit;
		}
		sqlite3_bind_text(sql_stmt, 1, sha, strlen(sha), NULL);
		sqlite3_bind_text(sql_stmt, 2, end, strlen(end), NULL);
		if (upstream)
			sqlite3_bind_text(sql_stmt, 3, upstream, strlen(upstream), NULL);

		if (reverts)
			sqlite3_bind_text(sql_stmt, 4, reverts, strlen(reverts), NULL);

		if (fixes)
			sqlite3_bind_text(sql_stmt, 5, fixes, strlen(fixes), NULL);

		ret = sqlite3_step(sql_stmt);
		if (ret != SQLITE_DONE) {
			fprintf(stderr, "Error inserting row %s\n", sqlite3_errmsg(database));
		}

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

	fprintf(stdout, "git head tag = %s\n", buf.ptr);

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

		snprintf(range1, sizeof(range1), "2.6.%d", x - 1);
		snprintf(range2, sizeof(range2), "2.6.%d", x);
		create_kernel_range_major(range1, range2);
	}
}

static int get_options(int argc, char *argv[])
{
	const char *env_string;

	env_string = getenv("CVEKERNELTREE");
	if (!env_string) {
		fprintf(stderr, "Error: Environment variable CVEERNELTREE must be set to point to\n");
		fprintf(stderr, "       the Linux kernel stable git repository directory.\n");
		return -1;
	}
	git_repo_location = strdup(env_string);



	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	fprintf(stdout, "%s version %s\n", PACKAGE_NAME, VERSION);

	ret = get_options(argc, argv);
	if (ret)
		return ret;

	fprintf(stdout, "Using stable kernel tree at %s\n", git_repo_location);

	ret = db_read_from_disk();
	if (ret) {
		// Database is not present, so let's initialize it ourself
		ret = db_init();
		if (ret)
			goto exit;
	}

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

	// Fill in the last little bit of -rc release information if we have it in the tree
	create_kernel_range_rc();

	git_shutdown();

	db_write_to_disk();

exit:
	free(git_repo_location);
	db_shutdown();
	return ret;
}
