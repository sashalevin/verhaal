// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024-2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <git2.h>
#include "ccan/list/list.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>	// Now we have 2 problems...

#include "terminal.h"
#include "verhaal.h"


// Find a pattern in a string.  If it is found, a pointer to the string is
// returned.  The return string is a new memory allocation and must be freed by
// the caller.  If the pattern is not found, NULL is returned.
//
// Surely there's a simpler way to do this, it just feels so "clunky"...
char *search_string(const char *string, const char *pattern)
{
	int ret;
	char *match= NULL;
	pcre2_code *re_pattern;
	int errornumber;
	pcre2_match_data *match_pattern;
	PCRE2_SIZE erroroffset;
	PCRE2_SPTR pcre_pattern = (PCRE2_SPTR8)pattern;

	// initialize our regular expression to what pcre2 wants to use
	re_pattern = pcre2_compile(pcre_pattern,
				   PCRE2_ZERO_TERMINATED, PCRE2_CASELESS,
				   &errornumber, &erroroffset, NULL);
	if (!re_pattern) {
		fprintf(stderr, "pcre regex for pattern '%s' was not created.\n",
			pattern);
		goto exit;
	}
	match_pattern = pcre2_match_data_create_from_pattern(re_pattern, NULL);

	// Try to find a match
	ret = pcre2_match(re_pattern, (PCRE2_SPTR8)string, strlen(string), 0, 0,
			  match_pattern, NULL);
	if (ret > 0) {
		// match found something!
		PCRE2_SIZE *ovector;

		ovector = pcre2_get_ovector_pointer(match_pattern);
		for (int i = 0; i < ret; ++i) {
			PCRE2_SPTR substring_start = (PCRE2_SPTR8)string + ovector[2*i];
			size_t substring_length = ovector[2*i+1] - ovector[2*i];

			//printf("	%2d: %.*s\n", i, (int)substring_length2, (char *)substring_start2);
			match = malloc(substring_length + 1);
			memcpy(match, substring_start, substring_length);
			match[substring_length] = 0x00;
		}
	}

	pcre2_match_data_free(match_pattern);
	pcre2_code_free(re_pattern);
exit:
	return match;
}


