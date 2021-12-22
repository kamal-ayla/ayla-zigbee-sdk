/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdlib.h>
#include <string.h>

#include <ayla/str_utils.h>


/*
 * Parse a string and expand any variables in the table.  Variable names start
 * with '$' and are case-insensitive.  dest and src may point to overlapping
 * memory regions.  dest will be NULL terminated if there is space remaining
 * in the buffer after variable expansion.  arg is a user-defined pointer
 * passed to the load function.
 * Returns the the number of characters written to dest on success, or -1
 * on failure.
 */
ssize_t str_expand_vars(const struct str_var *var_table,
	char *dest, size_t dest_size, const char *src, size_t src_len,
	void *arg)
{
	const char *cp;
	unsigned offset = 0;
	ssize_t rc;
	char in_buf[src_len + 1];
	const struct str_var *var;
	const struct str_var *match;

	/* Copy input to allow in-place expansion */
	strncpy(in_buf, src, src_len);
	in_buf[src_len] = '\0';
	cp = in_buf;

	while (*cp) {
		if (offset >= dest_size) {
			return -1;
		}
		/* Copy non-variable characters to out buffer */
		if (*cp != '$') {
regular_char:
			dest[offset++] = *cp++;
			continue;
		}
		++cp;
		if (*cp == '$') {
			goto regular_char; /* '$' used to escape a '$' */
		}
		/* Look up best variable match */
		match = NULL;
		for (var = var_table; var->name; ++var) {
			if (src_len - (cp - in_buf) >= var->name_len &&
			    !strncasecmp(cp, var->name, var->name_len)) {
				/* Match longest variable name in list */
				if (!match || match->name_len < var->name_len) {
					match = var;
				}
			}
		}
		if (match) {
			/* Expand the variable */
			rc = match->load(dest + offset,
				dest_size - offset, match->name, arg);
			if (rc < 0) {
				return -1;
			}
			cp += match->name_len;
			offset += rc;
		} else {
			/* Not a recognized variable, so restore the $ */
			dest[offset++] = '$';
		}
	}
	/* NULL terminate, if possible */
	if (offset < dest_size) {
		dest[offset] = '\0';
	}
	return offset;
}
