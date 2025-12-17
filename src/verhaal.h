// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024-2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
#ifndef __VERHAAL_H__
#define __VERHAAL_H__

#include <stdbool.h>
#include <git2.h>
#include "ccan/list/list.h"

#define VERSION_NAME_SIZE	20	// should fit the whole vX.Y.Z string size

/* A specific version, and if it is in mainline or not */
struct version {
	char name[VERSION_NAME_SIZE];	// name
	bool mainline;			// If this is a "mainline" version
	bool new;			// Not in the database yet
};

/*
 * Version ranges are the steps from one release to another, the granularity in
 * which we want to calculate commits in.  While we keep the individual release
 * versions in the database to lookup mainline/not_mainline info from, it is
 * these "ranges" that matter in how we spelunk through git and save git ids
 */
struct version_range {
	struct version from;		// git tag start
	struct version to;		// git tag end
	bool mainline;			// If this is a "mainline" range
	bool new;			// Not in the database yet
	struct list_head commits;	// commits for this range
};

// db.c
int db_init(void);
void db_shutdown(void);
int db_release_add(const struct version *v);
int db_range_add(const struct version_range *vr);
int db_fix_add(const char *invalid, const char *valid);
int db_commit_add(const char *sha, const char *release,
		  int mainline, const char *mainline_id,
		  const char *reverts, const char *fixes);
int db_write_to_disk(void);
void db_transaction_begin(void);
void db_transaction_end(void);
extern char *database_name;
extern bool db_is_in_memory;

// search.c
char *search_string(const char *string, const char *pattern);

// time.c
struct vh_timestamp;
struct vh_timestamp *time_start(const char *name);
double time_stop(struct vh_timestamp *time);

// versions.c
void version_add(const char *version, bool mainline);
void version_range_add(const char *from, const char *to, bool mainline);
void versions_create(void);
void for_each_range_do(int (*do_it_function)(struct version_range *vr));
extern int new_ranges;

// fixes.c
void fixes_init(void);
char *fix_translate(const char *fix);

// main.c
extern git_repository *git_repo;
git_repository *git_repo_get(void);
void git_repo_set_thread(git_repository *repo);
void git_repo_clear_thread(void);
__attribute__((__format__(printf, 1, 2))) int dbg(const char *fmt, ...);

#endif	// __VERHAAL_H__
