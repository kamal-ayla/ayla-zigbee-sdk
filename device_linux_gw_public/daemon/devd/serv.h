/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_SERV_H__
#define __AYLA_SERV_H__

#include <ayla/server.h>

extern char devd_sock_path[];
extern char devd_msg_sock_path[];

/*
 * Reverse REST request.  Handled as a standard server request, but with
 * additional data about the request and the response.
 */
struct serv_rev_req {
	struct server_req *req;	/* Server request */
	int cmd_id;		/* Reverse REST command ID */
	char *resource;		/* URI of the reverse REST command */
	char *resp_uri;		/* URI for the reverse REST response */
	int source;		/* Source ID of the reverse REST command */
	struct client_lan_reg *lan;	/* Optional pointer to LAN session */
};

void serv_init(void);

/*
 * Free a reverse REST command.  Normally done by the put_end function.
 */
void serv_rev_req_close(struct serv_rev_req *rev_req);

struct client_lan_reg;
void serv_json_cmd(json_t *cmd, struct client_lan_reg *);

/*
 * Perform a GET wifi_info.json and update devd state with the response.
 */
int serv_wifi_info_request(void);

/*
 * Parse an OTA json object and pick out the necessary pieces. Return error
 * if any parts are missing.
 */
int serv_ota_obj_parse(json_t *ota_obj, const char **ota_type,
		const char **checksum, const char **url, const char **ver,
		size_t *size);

#endif /* __AYLA_SERV_H__ */
