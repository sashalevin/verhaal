// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024-2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//

// search.c
char *search_string(const char *string, const char *pattern);

// time.c
struct vh_timestamp;
struct vh_timestamp *time_start(const char *name);
void time_stop(struct vh_timestamp *time);
