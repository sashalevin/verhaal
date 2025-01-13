// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * Slightly modified from Jason's original code by Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * All bugs are mine, not Jason's.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include "terminal.h"

// Check for the NO_COLOR environment variable as per the https://no-color.org/ "standard"
static bool color = true;

static bool no_color_test(void)
{
	static bool checked = false;
	const char *no_color;

	if (checked)
		return color;

	no_color = getenv("NO_COLOR");
	if (no_color && no_color[0] != '\0')
		color = false;

	checked = true;
	return color;
}

static bool color_mode(FILE *file)
{
	if (no_color_test() == false)
		return false;

	return isatty(fileno(file));
}

__attribute__((__format__(printf, 2, 0)))
static void filter_ansi(FILE *file, const char *fmt, va_list args)
{
	char *str = NULL;
	size_t len, i, j;

	if (color_mode(file)) {
		vfprintf(file, fmt, args);
		return;
	}

	len = vasprintf(&str, fmt, args);

	if (len >= 2) {
		for (i = 0; i < len - 2; ++i) {
			if (str[i] == '\x1b' && str[i + 1] == '[') {
				str[i] = str[i + 1] = '\0';
				for (j = i + 2; j < len; ++j) {
					if (isalpha(str[j]))
						break;
					str[j] = '\0';
				}
				str[j] = '\0';
			}
		}
	}
	for (i = 0; i < len; i = j) {
		fputs(&str[i], file);
		for (j = i + strlen(&str[i]); j < len; ++j) {
			if (str[j] != '\0')
				break;
		}
	}

	free(str);
}

void terminal_printf(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	filter_ansi(stdout, fmt, args);
	va_end(args);
}

void terminal_fprintf(FILE *file, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	filter_ansi(file, fmt, args);
	va_end(args);
}
