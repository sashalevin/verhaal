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

bool db_is_in_memory;

#define SCHEMA_VERSION	"001"		// Bump this if the schema changes

// We have a PRIMARY KEY although it is probably not needed because git ensures us of this anyway...
// FIXME, make release a foreign key to the releases table:
//	https://www.sqlite.org/foreignkeys.html
static const char *db_create_commits_sql =	"CREATE TABLE IF NOT EXISTS commits "	\
						"(id TEXT PRIMARY KEY NOT NULL, "	\
						" release TEXT NOT NULL, "		\
						" mainline INTEGER,"			\
						" mainline_id TEXT,"			\
						" reverts TEXT,"			\
						" fixes TEXT);";

static const char *db_create_indexes_sql =	"CREATE INDEX IF NOT EXISTS idx_commits_mainline_id ON commits(mainline_id);"	\
						"CREATE INDEX IF NOT EXISTS idx_commits_reverts ON commits(reverts);"		\
						"CREATE INDEX IF NOT EXISTS idx_commits_id_mainline ON commits(id, mainline);"	\
						"CREATE INDEX IF NOT EXISTS idx_commits_release ON commits(release);";

static const char *db_create_releases_sql =	"CREATE TABLE IF NOT EXISTS releases "	\
						"(release TEXT PRIMARY KEY NOT NULL, "	\
						" mainline INTEGER);";

static const char *db_create_ranges_sql =	"CREATE TABLE IF NOT EXISTS ranges "	\
						 "(version_from TEXT NOT NULL, "	\
						 " version_to TEXT NOT NULL, "		\
						 " mainline INTEGER);";

static const char *db_create_version_sql =	"CREATE TABLE IF NOT EXISTS version "	\
						 "(verhaal_version TEXT NOT NULL, "	\
						 " schema_version TEXT NOT NULL);";

// FIXME, make sha_valid a foreign key to the commits table:
//	https://www.sqlite.org/foreignkeys.html
static const char *db_create_fixes_sql =	"CREATE TABLE IF NOT EXISTS fixes "	\
						"(sha_invalid TEXT NOT NULL, "	\
						" sha_valid TEXT NOT NULL);";

static struct sqlite3 *database;

static const char *db_insert_release_sql = "INSERT INTO releases (release, mainline) VALUES (?, ?);";
int db_release_add(const struct version *v)
{
	const char *release = v->name;
	int mainline = v->mainline;
	sqlite3_stmt *sql_stmt = NULL;
	int ret;

	// Don't add an "old" version to the database
	if (v->new == false)
		return 0;

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

static const char *db_insert_range_sql = "INSERT INTO ranges (version_from, version_to, mainline) VALUES (?, ?, ?);";
int db_range_add(const struct version_range *vr)
{
	const char *from = vr->from.name;
	const char *to = vr->to.name;
	int mainline = vr->mainline;
	sqlite3_stmt *sql_stmt = NULL;
	int ret;

	// Don't add an "old" version to the database
	if (vr->new == false)
		return 0;

	dbg("%s: %10s %10s mainline=%d\n", __func__, from, to, mainline);

	ret = sqlite3_prepare(database, db_insert_range_sql, -1, &sql_stmt, NULL);
	if (ret) {
		fprintf(stderr, "Error preparing release sql statement %s\n",
			sqlite3_errmsg(database));
		return ret;
	}
	sqlite3_bind_text(sql_stmt, 1, from, strlen(from), NULL);
	sqlite3_bind_text(sql_stmt, 2, to, strlen(to), NULL);
	sqlite3_bind_int(sql_stmt, 3, mainline);

	ret = sqlite3_step(sql_stmt);
	if (ret != SQLITE_DONE)
		fprintf(stderr, "Error inserting range %s %s row %s\n", from, to, sqlite3_errmsg(database));

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

void db_transaction_begin(void)
{
	char *error;

	sqlite3_exec(database, "BEGIN TRANSACTION", NULL, NULL, &error);
}

void db_transaction_end(void)
{
	char *error;

	sqlite3_exec(database, "END TRANSACTION", NULL, NULL, &error);
}

static int create_table(const char *table_name, const char *sql)
{
	char *error;
	int ret;

	/* Create the releases table */
	ret = sqlite3_exec(database, sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error '%s' when attempting to create table %s in database file %s\n",
			error, table_name, database_name);
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	sqlite3_free(error);
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

	// Create the indexes when we shutdown so as to make the original inserts go faster
	ret = create_table("indexes", db_create_indexes_sql);
	if (ret)
		fprintf(stderr, "Problem creating indexes");

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

static int database_create(void)
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
	db_is_in_memory = true;

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

static int releases_callback(void *data, int argc, char **argv, char **column_name)
{
	const char *release;
	const char *mainline;
	bool mainline_bool;

	if (argc != 2) {
		terminal_fprintf(stdout, "    Database file '"
				 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
				 "' does not have the correct size of the releases table, creating a new one...\n",
				 database_name);
		return -1;
	}

	release = argv[0];
	mainline = argv[1];

	//printf("%s: release='%s'	mainline='%s'\n", __func__, release, mainline);
	if (!strcmp(mainline, "0"))
		mainline_bool = false;
	else
		mainline_bool = true;

	// Add this to memory.  It will NOT be written back to the database because the ->new flag
	// will not be set, so all is good.
	version_add(release, mainline_bool);
	return 0;
}

static int db_read_versions(void)
{
	int ret;
	char *error;

	const char *db_check_versions_sql = "SELECT * from releases;";
	ret = sqlite3_exec(database, db_check_versions_sql, releases_callback, 0, &error);
	if (ret != SQLITE_OK) {
		// Query did not work, so versions table is not there, so let's close this and
		// create a new one
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	return 0;
}

static int ranges_callback(void *data, int argc, char **argv, char **column_name)
{
	const char *from;
	const char *to;
	const char *mainline;
	bool mainline_bool;

	if (argc != 3) {
		terminal_fprintf(stdout, "    Database file '"
				 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
				 "' does not have the correct size of the ranges table, creating a new one...\n",
				 database_name);
		return -1;
	}

	from = argv[0];
	to = argv[1];
	mainline = argv[2];

	// printf("%s: from='%s'	to='%s'	mainline='%s'\n", __func__, from, to, mainline);
	if (!strcmp(mainline, "0"))
		mainline_bool = false;
	else
		mainline_bool = true;

	// Add this to memory.  It will NOT be written back to the database because the ->new flag
	// will not be set, so all is good.
	version_range_add(from, to, mainline_bool);
	return 0;
}

static int db_read_ranges(void)
{
	int ret;
	char *error;

	const char *db_check_versions_sql = "SELECT * from ranges;";
	ret = sqlite3_exec(database, db_check_versions_sql, ranges_callback, 0, &error);
	if (ret != SQLITE_OK) {
		// Query did not work, so versions table is not there, so let's close this and
		// create a new one
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	return 0;
}

static int version_callback(void *data, int argc, char **argv, char **column_name)
{
	const char *version;
	const char *schema;

	if (argc != 2) {
		terminal_fprintf(stdout, "    Database file '"
				 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
				 "' does not have the correct size of the versions table, creating a new one...\n",
				 database_name);
		return -1;
	}

	version = argv[0];
	schema = argv[1];

	if (strcmp(schema, SCHEMA_VERSION)) {
		terminal_fprintf(stdout, "    Database contains schema '"
				 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
				 "' which does not match our current schema version '"
				 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
				 "' so starting over...\n", schema, SCHEMA_VERSION);
		return -1;
	}
	terminal_fprintf(stdout, "    Database created with version '"
			 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
			 "' but identical schema version '"
			 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
			 "', so all is fine.\n",
			 version, schema);
	return 0;
}

static int database_check(void)
{
	FILE *db_file;
	char *error;
	int ret;

	// Check to see if the database is already here on disk
	db_file = fopen(database_name, "r");
	if (!db_file) {
		// database is not present, so let's start over!
		terminal_fprintf(stdout, "    Database file '"
				 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
				 " is not found, creating a new one...\n",
				 database_name);
		return -1;
	}
	fclose(db_file);

	// File is present, so let's open it and do some checks...
	ret = sqlite3_open(database_name, &database);
	if (ret != SQLITE_OK) {
		terminal_fprintf(stdout, "    Database file '"
				 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT
				 " does not seem to be a valid database at all, creating a new one...\n",
				 database_name);
		return ret;
	}

	// Seems like a valid database, so let's see if the version table is present and if so check
	// the schema.
	const char *db_check_versions_sql = "SELECT * from version;";
	ret = sqlite3_exec(database, db_check_versions_sql, version_callback, 0, &error);
	if (ret != SQLITE_OK) {
		// Query did not work, so versions table is not there, so let's close this and
		// create a new one
		sqlite3_free(error);
		sqlite3_close(database);
		return ret;
	}

	// Good schema!  So we can "know" that the tables and indexes are set up properly, so let's
	// read in the ranges that we currently have so we can "prepopulate" that information so we
	// only know what new versions and ranges we need to care about
	db_read_versions();
	db_read_ranges();

	// Return success so we just keep what we have on the disk
	return 0;
}

static int database_init(void)
{
	int ret;

	// Check to see if the database is already here and if so, set up the proper pointers
	ret = database_check();
	if (!ret)
		return ret;

	terminal_fprintf(stdout, "    Creating database from scratch, sorry for the delay...\n");

	// Database check failed, so let's build it all from scratch!
	return database_create();
}

static int releases_table_init(void)
{
	return create_table("releases", db_create_releases_sql);
}

static int ranges_table_init(void)
{
	return create_table("ranges", db_create_ranges_sql);
}

static int commits_table_init(void)
{
	char *error;
	int ret;

	ret = create_table("commits", db_create_commits_sql);
	if (ret)
		return ret;

	// Stick in the "first" commit as we have to do it by hand for some
	// reason because git doesn't like showing it for us...
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
	return create_table("fixes", db_create_fixes_sql);
}

static int version_table_init(void)
{
	char *error;
	int ret;

	ret = create_table("version", db_create_version_sql);
	if (ret)
		return ret;

	// Write the program version to the database.
	const char *db_initial_commit_sql = "INSERT INTO version (verhaal_version, schema_version) "
					    "VALUES ('"VERSION"', '"SCHEMA_VERSION"');";
	ret = sqlite3_exec(database, db_initial_commit_sql, 0, 0, &error);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "Error '%s' adding version to database %s\n",
			error, database_name);
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

	// If this is on disk, no need to initialize anything else as it's all good
	if (!db_is_in_memory)
		return 0;

	ret = releases_table_init();
	if (ret)
		return ret;

	ret = ranges_table_init();
	if (ret)
		return ret;

	ret = fixes_table_init();
	if (ret)
		return ret;

	ret = version_table_init();
	if (ret)
		return ret;

	return commits_table_init();
}

void db_shutdown(void)
{
	if (db_is_in_memory)
		db_write_to_disk();
	sqlite3_close(database);
}


