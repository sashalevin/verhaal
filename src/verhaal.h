// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024-2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
#ifndef __VERHAAL_H__
#define __VERHAAL_H__

#include <git2.h>

// search.c
char *search_string(const char *string, const char *pattern);

// time.c
struct vh_timestamp;
struct vh_timestamp *time_start(const char *name);
void time_stop(struct vh_timestamp *time);

// versions.c
void versions_create(void);

// main.c
extern git_repository *git_repo;
__attribute__((__format__(printf, 1, 2))) int dbg(const char *fmt, ...);
int db_release_add(const char *release, int mainline);

#endif	// __VERHAAL_H__
