// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024-2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
#ifndef __VERHAAL_H__
#define __VERHAAL_H__

#include <stdbool.h>
#include <git2.h>

// db.c
int db_init(void);
void db_shutdown(void);
int db_release_add(const char *release, int mainline);
int db_fix_add(const char *invalid, const char *valid);
int db_commit_add(const char *sha, const char *release,
		  int mainline, const char *mainline_id,
		  const char *reverts, const char *fixes);
int db_write_to_disk(void);
extern char *database_name;

// search.c
char *search_string(const char *string, const char *pattern);

// time.c
struct vh_timestamp;
struct vh_timestamp *time_start(const char *name);
double time_stop(struct vh_timestamp *time);

// versions.c
void versions_create(void);
void for_each_range_do(int (*do_it_function)(const char *major, const char *minor, bool mainline));

// fixes.c
void fixes_init(void);
char *fix_translate(const char *fix);

// main.c
extern git_repository *git_repo;
__attribute__((__format__(printf, 1, 2))) int dbg(const char *fmt, ...);

#endif	// __VERHAAL_H__
