/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_VAR_EXPANSION_H__
#define __AYLA_VAR_EXPANSION_H__

/*
 * Structure for variable expansion entries.  The load function is passed the
 * variable name and a user-defined argument, and should write up to buf_size
 * bytes of variable data to buf.  If successful, the number of bytes should
 * be returned.  If loading fails, return -1.
 */
struct str_var {
	const char *name;
	size_t name_len;
	ssize_t (*load)(char *buf, size_t buf_size, const char *var, void *arg);
};

/*
 * Convenience macros for populating a variable table.
 *
 * Example usage:
 *    const struct str_var var_table[] = {
 *       STR_VAR_DECL(var1, load_var1)
 *       STR_VAR_DECL(var2, load_var2)
 *       STR_VAR_END
 *    };
 */
#define STR_VAR_DECL(name, load_func)	\
	{ #name, sizeof(#name) - 1, load_func },
#define STR_VAR_END	\
	{ NULL }

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
	void *arg);

#endif /* __AYLA_VAR_EXPANSION_H__ */

