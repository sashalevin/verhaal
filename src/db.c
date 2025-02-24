// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024-2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
// Database specific stuff

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sqlite3.h>

#include "verhaal.h"
#include "terminal.h"

char *database_name;

// We have a PRIMARY KEY although it is probably not needed because git ensures us of this anyway...
// FIXME, make release a foreign key to the releases table:
//	https://www.sqlite.org/foreignkeys.html
static const char *db_create_commits_sql =	"CREATE TABLE IF NOT EXISTS commits "	\
						"(id TEXT PRIMARY KEY NOT NULL, "	\
						" release TEXT NOT NULL, "		\
						" mainline INTEGER,"			\
						" mainline_id TEXT,"			\
						" reverts TEXT,"			\
						" fixes TEXT);"				\
						"CREATE INDEX IF NOT EXISTS idx_commits_mainline_id ON commits(mainline_id);"	\
						"CREATE INDEX IF NOT EXISTS idx_commits_reverts ON commits(reverts);"		\
						"CREATE INDEX IF NOT EXISTS idx_commits_id_mainline ON commits(id, mainline);"	\
						"CREATE INDEX IF NOT EXISTS idx_commits_release ON commits(release);";

static const char *db_create_releases_sql =	"CREATE TABLE IF NOT EXISTS releases "	\
						"(release TEXT PRIMARY KEY NOT NULL, "	\
						" mainline INTEGER);";

// FIXME, make sha_valid a foreign key to the commits table:
//	https://www.sqlite.org/foreignkeys.html
static const char *db_create_fixes_sql =	"CREATE TABLE IF NOT EXISTS fixes "	\
						"(sha_invalid TEXT NOT NULL, "	\
						" sha_valid TEXT NOT NULL);";

static struct sqlite3 *database;

static const char *db_insert_release_sql = "INSERT INTO releases (release, mainline) VALUES (?, ?);";
int db_release_add(const char *release, int mainline)
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

static const char *db_insert_fixes_sql = "INSERT INTO fixes (sha_invalid, sha_valid) VALUES (?, ?);";
int db_fix_add(const char *invalid, const char *valid)
{
	sqlite3_stmt *sql_stmt = NULL;
	int ret;

	dbg("%s: %40s %40s\n", __func__, invalid, valid);

	ret = sqlite3_prepare(database, db_insert_fixes_sql, -1, &sql_stmt, NULL);
	if (ret) {
		fprintf(stderr, "Error preparing fixes sql statement %s\n",
			sqlite3_errmsg(database));
		return ret;
	}
	sqlite3_bind_text(sql_stmt, 1, invalid, strlen(invalid), NULL);
	sqlite3_bind_text(sql_stmt, 2, valid, strlen(valid), NULL);

	ret = sqlite3_step(sql_stmt);
	if (ret != SQLITE_DONE)
		fprintf(stderr, "Error inserting fixes %s row %s\n", invalid, sqlite3_errmsg(database));

	ret = sqlite3_finalize(sql_stmt);

	return ret;
}

static const char *db_insert_sql = "INSERT INTO commits (id, release, mainline, mainline_id, reverts, fixes) VALUES (?, ?, ?, ?, ?, ?);";
int db_commit_add(const char *sha, const char *release,
		  int mainline, const char *mainline_id,
		  const char *reverts, const char *fixes)
{
	sqlite3_stmt *sql_stmt = NULL;
	int ret;

	dbg("%s: %s\n", __func__, sha);

	// Save it in the database
	ret = sqlite3_prepare(database, db_insert_sql, -1, &sql_stmt, NULL);
	if (ret) {
		fprintf(stderr, "Error preparing sql statement %s\n",
			sqlite3_errmsg(database));
		return ret;
	}
	sqlite3_bind_text(sql_stmt, 1, sha, strlen(sha), NULL);
	sqlite3_bind_text(sql_stmt, 2, release, strlen(release), NULL);
	sqlite3_bind_int(sql_stmt, 3, mainline);

	if (mainline_id)
		sqlite3_bind_text(sql_stmt, 4, mainline_id, strlen(mainline_id), NULL);

	if (reverts)
		sqlite3_bind_text(sql_stmt, 5, reverts, strlen(reverts), NULL);

	if (fixes)
		sqlite3_bind_text(sql_stmt, 6, fixes, strlen(fixes), NULL);

	ret = sqlite3_step(sql_stmt);
	if (ret != SQLITE_DONE)
		fprintf(stderr, "Error inserting commit %s row %s\n", sha, sqlite3_errmsg(database));

	ret = sqlite3_finalize(sql_stmt);

	return ret;
}

// Write the in-memory database out to disk
int db_write_to_disk(void)
{
	int ret;
	double seconds;
	sqlite3 *file;
	sqlite3_backup *backup;
	struct vh_timestamp *foo;

	foo = time_start("Write data to disk");

	terminal_fprintf(stdout, "  Writing to database file '"
			 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT "'\n", database_name);

	ret = sqlite3_open(database_name, &file);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error, unable to open database file %s\n", database_name);
		goto exit;
	}

	backup = sqlite3_backup_init(file, "main", database, "main");
	if (backup) {
		sqlite3_backup_step(backup, -1);
		sqlite3_backup_finish(backup);
	}
	ret = sqlite3_errcode(file);

	sqlite3_close(file);
exit:
	seconds = time_stop(foo);
	terminal_fprintf(stdout, "    Database write to disk took "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", seconds);
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

static int database_init(void)
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

	// Configure SQLite for "optimal" performance
	const char *pragmas[] = {
		"PRAGMA synchronous = OFF",     // Safe for in-memory DB
		"PRAGMA cache_size = -2000000", // 2GB cache
		"PRAGMA temp_store = MEMORY",   // In-memory temp storage
		"PRAGMA foreign_keys = ON",     // Enable foreign key support
		"PRAGMA journal_mode = WAL",	// Enable Write-Ahead-Logging https://www.sqlite.org/wal.html  Might not do much for an in-memory db, but let's be safe
		NULL
	};

	for (const char **pragma = pragmas; *pragma != NULL; pragma++) {
		ret = sqlite3_exec(database, *pragma, 0, 0, &error);
		if (ret != SQLITE_OK) {
			fprintf(stderr, "Error setting pragma %s: %s\n", *pragma, error);
			sqlite3_free(error);
			sqlite3_close(database);
			return ret;
		}
	}

	return ret;
}

static int releases_table_init(void)
{
	char *error;
	int ret;

	/* Create the releases table */
	ret = sqlite3_exec(database, db_create_releases_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error creating release table %s %s\n",
			database_name, error);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	sqlite3_free(error);
	return ret;
}

static int commits_table_init(void)
{
	char *error;
	int ret;

	/* Create the tables if it's not been initialized yet */
	ret = sqlite3_exec(database, db_create_commits_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error creating commits table %s %s\n",
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

static int fixes_table_init(void)
{
	char *error;
	int ret;

	/* Create the releases table */
	ret = sqlite3_exec(database, db_create_fixes_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error creating release table %s %s\n",
			database_name, error);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	sqlite3_free(error);
	return ret;
}

int db_init(void)
{
	int ret;

	ret = database_init();
	if (ret)
		return ret;

	ret = releases_table_init();
	if (ret)
		return ret;

	ret = fixes_table_init();
	if (ret)
		return ret;

	return commits_table_init();
}

void db_shutdown(void)
{
	sqlite3_close(database);
}


