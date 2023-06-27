/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_GATEWAY_CLIENT_H__
#define __AYLA_GATEWAY_CLIENT_H__

extern const char * const gateway_ops[];

/*
 * Initialize the gateway subsystem
 */
int gateway_init(void);

/*
 * Remove all DSN to node mappings from the lookup table.
 */
void gateway_mapping_delete_all(void);

/*
 * Convert address to dsn
 */
const char *gateway_addr_to_dsn(const char *addr);

/*
 * Convert dsn to address
 */
const char *gateway_dsn_to_addr(const char *dsn);

/*
 * Take a json object and convert the address in it to DSN
 */
int gateway_convert_address_to_dsn(json_t *status_info_j);

/*
 * Convert a property info structure from appd. Take the subdevice_key,
 * template_key and name, into a delimited prop name. Optionally a bkup_j
 * object can be given to backup the information.
 */
int gateway_prop_info_to_name(json_t *prop_info_j, json_t *bkup_j);

/*
 * Take a json object and convert the DSN in it to address
 */
int gateway_convert_dsn_to_address(json_t *info_j);


/*
 * Setup an internal gw cmd to echo node property updates
 * if other destinations exist
 */
void gateway_node_prop_prepare_echo(struct device_state *dev, json_t *elem_j,
	int source);

void gateway_video_stream_request_statemachine(struct timer* timer);
int start_video_stream_request(struct device_state *dev, const char* addr);

#endif /*  __AYLA_GATEWAY_CLIENT_H__ */

