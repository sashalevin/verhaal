// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
// "replacement" for the old-school "abuse the filesystem as a database" tool that we use to store
// all Linux kernel mainline and stable kernel commits in so that we can "quickly" search them.
//
// Instead of using the database and grep we do it all in a sql database and then we can search it
// with some simple sql statements.  Cuts time to search from about .6 seconds to .01 seconds on my
// semi-slow-storage device.
//
// This is the "create the database" program.  It does it all in ram and then writes it out to disk
// when finished as that's much faster.  It recreates the world, all at once.  Ideally we should
// "just" add the new records when they are found, but that's for another day...
//

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sqlite3.h>
#include <git2.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>	// Now we have 2 problems...

static git_repository *git_repo;

static const char *git_repo_location = "/home/gregkh/linux/stable/linux-stable/";

static const char *database_name = "commits.db";

// We have a PRIMARY KEY although it is probably not needed because git ensures us of this anyway...
static const char *db_create_sql =	"CREATE TABLE IF NOT EXISTS commits "	\
					"(id TEXT PRIMARY KEY NOT NULL, "	\
					" release TEXT NOT NULL, "		\
					" mainline_id TEXT,"			\
					" fixes TEXT);";

// TODO : add logic to parse Fixes tags as well and put them in the "fixes" field  Will make some
// other searches that dyad runs MUCH faster

static struct sqlite3 *database;

static int db_create(void)
{
	return 0;
}

static int db_init(void)
{
	char *error;
	int ret;

	// We open an in-memory database to create everything, and then write it all out at the very
	// end.  Hack, yes, but fast, blazingly.
	ret = sqlite3_open(":memory:", &database);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error opening database %s %s\n",
			database_name, sqlite3_errmsg(database));
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

			// FIXME: Now do the second search of the line for the sha
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

static const char *db_insert_sql = "INSERT INTO commits (id, release) VALUES (?, ?);";
static const char *db_insert_mainline_sql = "INSERT INTO commits (id, release, mainline_id) VALUES (?, ?, ?);";

static int create_kernel_range(const char *start, const char *end, bool minor)
{
	char *upstream = NULL;
	char range[256];
	git_oid oid;
	git_revwalk *walker;
	int ret;

	//fprintf(stdout, "%s: start=%s, end=%s, minor=%d\n", __func__, start, end, minor);

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

	fprintf(stdout, "saving v%s..v%s\n", start, end);

	while (!git_revwalk_next(&oid, walker)) {
		char sha[256];
		sqlite3_stmt *sql_stmt = NULL;

		git_oid_tostr(sha, sizeof(sha), &oid);

		// If this is a minor range, search the changelog message to figure out if this is
		// an upstream id, and if so, what it is and then save it off.
		// FIXME also save the Fixes: tag
		if (minor) {
			const char *message;
			git_commit *commit;

			ret = git_commit_lookup(&commit, git_repo, &oid);
			if (ret) {
				fprintf(stderr, "git message lookup for %s failed\n", sha);
				continue;
			}

			message = git_commit_message(commit);
			upstream = find_upstream(message);
			git_commit_free(commit);
		}

		if (upstream) {
			//printf("	upstream=%s\n", upstream);
			ret = sqlite3_prepare(database, db_insert_mainline_sql, -1, &sql_stmt, NULL);
			if (ret) {
				fprintf(stderr, "Error preparing sql statement %s\n",
					sqlite3_errmsg(database));
				goto exit;
			}
			sqlite3_bind_text(sql_stmt, 1, sha, strlen(sha), NULL);
			sqlite3_bind_text(sql_stmt, 2, end, strlen(end), NULL);
			sqlite3_bind_text(sql_stmt, 3, upstream, strlen(upstream), NULL);
			ret = sqlite3_step(sql_stmt);
			if (ret != SQLITE_DONE) {
				fprintf(stderr, "Error inserting row %s\n", sqlite3_errmsg(database));
			}
			free(upstream);
		} else {
			// Just commit the sha and version as that's all we know here
			ret = sqlite3_prepare(database, db_insert_sql, -1, &sql_stmt, NULL);
			if (ret) {
				fprintf(stderr, "Error preparing sql statement %s\n",
					sqlite3_errmsg(database));
				goto exit;
			}
			sqlite3_bind_text(sql_stmt, 1, sha, strlen(sha), NULL);
			sqlite3_bind_text(sql_stmt, 2, end, strlen(end), NULL);
			ret = sqlite3_step(sql_stmt);
			if (ret != SQLITE_DONE) {
				fprintf(stderr, "Error inserting row %s\n", sqlite3_errmsg(database));
			}
		}
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

static void loop_through_y(int major, int minor)
{
	char str[256];
	char range1[256];
	char range2[256];
	int y;

	for (y = 1; y < 400; ++y) {
		snprintf(str, sizeof(str), "v%d.%d.%d", major, minor, y);
		if (!is_valid_release(str)) {
			// printf("%s is NOT a valid release\n", str);
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
			// printf("%s is NOT a valid release\n", str);
			continue;
		}
		// printf("%s is a valid release\n", str);

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
			//printf("%s is NOT a valid release\n", str);
			continue;
		}

		for (int y = 1; y < 101; ++y) {
			snprintf(str, sizeof(str), "v2.6.%d.%d", x, y);
			if (!is_valid_release(str)) {
				//printf("%s is NOT a valid release\n", str);
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

int main(void)
{
	int ret;

	ret = db_init();
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

	// Now take the "last" release, and do all of the -rc releases to catch them.
	// FIXME

	git_shutdown();

	db_write_to_disk();

exit:
	db_shutdown();
	return ret;
}
