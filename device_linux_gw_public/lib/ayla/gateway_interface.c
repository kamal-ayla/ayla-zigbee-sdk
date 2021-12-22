#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/gateway_interface.h>

const char * const gateway_ops[] = JINT_GATEWAY_OP_NAMES;

/*
 * For INTERNAL Ayla use ONLY
 * Break property name into subdevice_key, template_key, and template prop
 * NOTE: This function is destructive to prop_name
 */
int gateway_break_up_node_prop_name(char *prop_name, const char **subdevice_key,
			const char **template_key, const char **template_prop)
{
	const char delimiter[] = GATEWAY_PROPNAME_DELIM;
	char *saveptr;

	*subdevice_key  = strtok_r(prop_name, delimiter, &saveptr);
	if (*subdevice_key == NULL) {
bad_name:
		log_warn("bad node prop name: %s", prop_name);
		return -1;
	}
	*template_key  = strtok_r(NULL, delimiter, &saveptr);
	if (*template_key == NULL) {
		goto bad_name;
	}
	*template_prop = strtok_r(NULL, delimiter, &saveptr);
	if (*template_prop == NULL) {
		goto bad_name;
	}
	return 0;
}

/*
 * For INTERNAL Ayla use ONLY
 * Given an operation string, get the gateway opcode
 */
enum ayla_gateway_op gateway_op_get(const char *str)
{
	int i;

	for (i = 0; i < ARRAY_LEN(gateway_ops); i++) {
		if (gateway_ops[i] && !strcmp(str, gateway_ops[i])) {
			return (enum ayla_gateway_op)i;
		}
	}
	return AG_NOP;
}
