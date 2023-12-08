/*
 * Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/md5.h>
#include <sys/stat.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/ayla_interface.h>
#include <ayla/json_interface.h>
#include <ayla/gateway_interface.h>
#include <ayla/json_parser.h>
#include <ayla/hashmap.h>
#include <ayla/timer.h>
#include <ayla/conf_io.h>
#include <ayla/file_io.h>

#include "ds.h"
#include "ds_client.h"
#include "dapi.h"
#include "serv.h"
#include "ops_devd.h"
#include "props_client.h"
#include "app_if.h"
#include "gateway_if.h"
#include "gateway_client.h"
#include "vt_node_list.h"
#include "video_stream_ds.h"

#define LAN_CONN_STATUS_URL	"node/conn_status.json"
#define LAN_NODE_PROP_SEND_URL	"node/property/datapoint.json"
#define LAN_NODE_PROP_ACK_URL	"node/property/datapoint/ack.json"
#define BATCH_NODE_DPS_URL	"https://%s/%s/dsns/%s/batch_datapoints.json" \
				"?nodes=1"

#define MAX_PROP_NAME_LEN	255
#define CLOUD_PROP_NAME_FORMAT	"%s" GATEWAY_PROPNAME_DELIM \
				"%s" GATEWAY_PROPNAME_DELIM "%s"

enum gateway_client_state {
	GCS_NOP = 0,
	GCS_NODE_ADD,
	GCS_NODE_UPDATE,
	GCS_NODE_REMOVE,
	GCS_CONN_STATUS,
	GCS_PROP_SEND,
	GCS_PROP_BATCH_SEND,
	GCS_GET_PROP,
	GCS_GET_ALL_PROPS,
	GCS_GET_TODEV_PROPS,
	GCS_AUTO_ECHO,
	GCS_NODE_RST_RESULT,
	GCS_NODE_OTA_RESULT,
	GCS_NODE_OTA_URL_FETCH,
	GCS_NODE_OTA_LOCAL_FETCH,
	GCS_NODE_OTA_REMOTE_FETCH,
	GCS_PUT_ACK,
	GCS_NODE_REG_RESULT,
};

/* Initial element size for dsn -> node_addr and node_addr -> dsn hash map */
#define GATEWAY_HASH_SIZE_INIT		32

/*
 * Gateway information for gateway operations
 */
struct gateway_cmd {
	json_t *info_j;
	enum ayla_gateway_op op;
};

static const struct op_funcs gw_op_handlers[];

static struct hashmap gw_dsn_to_addr;
static struct hashmap gw_addr_to_dsn;

HASHMAP_FUNCS_CREATE(gateway, const char, const char);

char req_json_paylod_copy[1024];

/*
 * This function should never be called.
 */
static int gateway_nop_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	log_err("bad call to nop");

	ASSERT_NOTREACHED();
	return 1;
}

/*
 * Free a gcmd structure
 */
static void gateway_free_gateway_cmd(void *arg)
{
	struct gateway_cmd *gcmd = arg;

	if (gcmd && gcmd->info_j) {
		json_decref(gcmd->info_j);
	}
	free(gcmd);
}

/*
 * Generic function when a gateway operation finishes.
 */
static int gateway_generic_op_finished(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;

	gateway_free_gateway_cmd(gcmd);
	return 0;
}

/*
 * Take a json object and convert the address in it to DSN
 */
int gateway_convert_address_to_dsn(json_t *info_j)
{
	const char *address;
	const char *dsn;

	if (json_get_string(info_j, "dsn")) {
		/* dsn already exists, no need to convert */
		return 0;
	}
	address = json_get_string(info_j, "address");
	if (!address) {
		log_warn("missing address");
		return -1;
	}
	dsn = gateway_addr_to_dsn(address);
	if (!dsn) {
		log_warn("couldn't find dsn for address %s", address);
		return -1;
	}
	json_object_del(info_j, "address");
	json_object_set_new(info_j, "dsn", json_string(dsn));

	return 0;
}

/*
 * Take a json object and convert the DSN in it to address
 */
int gateway_convert_dsn_to_address(json_t *info_j)
{
	const char *dsn;
	const char *address;

	if (json_get_string(info_j, "address")) {
		/* address already exists, no need to convert */
		return 0;
	}
	dsn = json_get_string(info_j, "dsn");
	if (!dsn) {
		log_warn("missing dsn");
		return -1;
	}
	address = gateway_dsn_to_addr(dsn);
	if (!address) {
		log_warn("couldn't find address for dsn %s", dsn);
		return -1;
	}
	json_object_del(info_j, "dsn");
	json_object_set_new(info_j, "address", json_string(address));

	return 0;
}

/*
 * Initialize gateway node connection status message
 */
static int gateway_conn_status_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	u8 targets;
	const char *node_dsn;
	char lan_link[CLIENT_LAN_MAX_URL_LEN];
	struct client_lan_reg *lan;
	int i;
	int rc;
	json_t *connection_j;

	for (i = 0; i < json_array_size(gcmd->info_j); i++) {
		if (gateway_convert_address_to_dsn(json_array_get(gcmd->info_j,
		    i))) {
			return -1;
		}
	}
	connection_j = json_object();
	REQUIRE(connection_j, REQUIRE_MSG_ALLOCATION);
	json_object_set(connection_j, "connection", gcmd->info_j);
	targets = op_cmd->dests_target;
	if (targets > 1) {
		/*
		 * Post to LAN clients first.
		 */
		snprintf(lan_link, sizeof(lan_link), "%s%s",
		    LAN_CONN_STATUS_URL, op_cmd->echo ? "?echo=true" : "");
		for (i = 1, lan = client_lan_reg; i < CLIENT_LAN_REGS;
		    i++, lan++) {
			if (!(targets & BIT(i))) {
				continue;
			}
			if (!lan->uri[0]) {
				log_err("missing lan %d", i);
				ops_devd_mark_results(op_cmd, BIT(i), false);
				continue;
			}
			rc = client_lan_post(dev, lan, connection_j, lan_link,
			    &op_cmd->err_type);
			ops_devd_mark_results(op_cmd, BIT(i), !rc);
		}
	}
	if (!(targets & DEST_ADS)) {
		json_decref(connection_j);
		return 1;
	}
	/*
	 * Send to ADS
	 */
	json_object_set(connection_j, "connection",
	    json_array_get(gcmd->info_j, 0));
	node_dsn = json_get_string(json_array_get(gcmd->info_j, 0), "dsn");
	snprintf(link, DS_CLIENT_LINK_MAX_LEN,
	    "https://%s/%s/dsns/%s/nodes/%s/conn_status.json",
	    dev->ads_host, ADS_API_VERSION, dev->dsn, node_dsn);

	ds_client_data_init_json(&info->req_data, connection_j);
	json_decref(connection_j);
	info->init = 1;
	*method = HTTP_PUT;

	return 0;
}

/*
 * Initialize gateway hash map.
 */
static int gateway_hash_init(void)
{
	int rc = 0;

	rc |= hashmap_init(&gw_dsn_to_addr, hashmap_hash_string,
	    hashmap_compare_string, GATEWAY_HASH_SIZE_INIT);
	rc |= hashmap_init(&gw_addr_to_dsn, hashmap_hash_string,
	    hashmap_compare_string, GATEWAY_HASH_SIZE_INIT);
	return rc;
}

/*
 * Convert address to DSN
 */
const char *gateway_addr_to_dsn(const char *addr)
{
	return gateway_hashmap_get(&gw_addr_to_dsn, addr);
}

/*
 * Convert DSN to address
 */
const char *gateway_dsn_to_addr(const char *dsn)
{
	return gateway_hashmap_get(&gw_dsn_to_addr, dsn);
}

/*
 * Remove a DSN to address mapping.
 * Returns 0 on success or -1 if entry isn't found.
 *
 */
static int gateway_mapping_delete(const char *dsn, const char *addr)
{
	const char *dsn_data;
	const char *addr_data;

	addr_data = gateway_hashmap_remove(&gw_dsn_to_addr, dsn);
	dsn_data = gateway_hashmap_remove(&gw_addr_to_dsn, addr);
	if (!dsn_data && !addr_data) {
		return -1;
	}
	free((void *)dsn_data);
	free((void *)addr_data);
	return 0;
}

/*
 * Add a DSN, addr mapping to the hash tables
 * Returns 0 on success.
 * Returns -1 if an error occurred.
 */
static int gateway_mapping_add(const char *dsn, const char *addr)
{
	char *dsn_data = strdup(dsn);
	char *addr_data = strdup(addr);
	if (!dsn_data || !addr_data) {
		goto error;
	}

	/* Remove any existing entries for this DSN and address */
	gateway_mapping_delete(dsn, addr);
	RemoveNode(addr);
	/* Add new hash entries for bi-directional lookup */
	if (!gateway_hashmap_put(&gw_dsn_to_addr, dsn_data, addr_data)) {
		goto error;
	}
	if (!gateway_hashmap_put(&gw_addr_to_dsn, addr_data, dsn_data)) {
		gateway_hashmap_remove(&gw_dsn_to_addr, dsn);
		goto error;
	}
	return 0;
error:
	free(dsn_data);
	free(addr_data);
	return -1;
}

/*
 * Remove all DSN to node mappings from the lookup table.
 */
void gateway_mapping_delete_all(void)
{
	struct hashmap_iter *iter;

	log_debug("removing all nodes");
	/* Free the data pointer of each map (used as the key of the other) */
	for (iter = hashmap_iter(&gw_dsn_to_addr); iter;
	    iter = hashmap_iter_next(&gw_dsn_to_addr, iter)) {
		free((void *)gateway_hashmap_iter_get_data(iter));
	}
	for (iter = hashmap_iter(&gw_addr_to_dsn); iter;
	    iter = hashmap_iter_next(&gw_addr_to_dsn, iter)) {
		free((void *)gateway_hashmap_iter_get_data(iter));
	}
	hashmap_reset(&gw_dsn_to_addr);
	hashmap_reset(&gw_addr_to_dsn);
}

/*
 * Response to node factory reset succeeded
 */
static int gateway_node_rst_success(struct ops_devd_cmd *op_cmd,
				    struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	int success;
	const char *addr;
	const char *dsn;

	addr = json_get_string(gcmd->info_j, "address");
	json_get_int(gcmd->info_j, "success", &success);

	if (!success) {
		return 0;
	}
	/* remove the dsn/addr mapping */
	dsn = gateway_addr_to_dsn(addr);
	if (!dsn) {
		return 0;
	}
	log_debug("delete node: %s --> %s", dsn, addr);
	if (!gateway_mapping_delete(dsn, addr)) {
		RemoveNode(addr);
		conf_save();
	}
	return 0;
}

/*
 * Get the MD5 Checksum of file
 */
int gateway_calc_md5_of_file(int fd, char *md5_byte_arr, size_t len)
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	unsigned char data[1024];
	MD5_CTX dl_md5;
	size_t bytes;
	size_t off;
	int i;

	if (MD5_Init(&dl_md5) != 1) {
		log_err("md5 init failed");
		return -1;
	}
	while ((bytes = read(fd, data, sizeof(data))) > 0) {
		MD5_Update(&dl_md5, data, bytes);
	}
	if (bytes == -1) {
		log_err("read err %m");
		return -1;
	}
	if (MD5_Final(digest, &dl_md5) != 1) {
		log_err("md5 final failed");
		return -1;
	}
	for (off = 0, i = 0; i < sizeof(digest); i++) {
		if (off >= len) {
			log_err("buffer too short");
			return -1;
		}
		off += snprintf(md5_byte_arr + off, len - off, "%2.2x",
		    digest[i]);
	}
	return 0;
}

/*
 * Successfully fetched the Node OTA image
 */
static int gateway_node_ota_fetch_success(struct ops_devd_cmd *op_cmd,
				    struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	char md5_read[33];
	int fd;
	json_t *cmd_data_j;
	json_t *ota_data_j;
	const char *save_location;
	const char *ota_type;
	const char *checksum;
	const char *url;
	const char *ver;
	size_t file_size;
	size_t size;
	int rc;

	cmd_data_j = json_object_get(gcmd->info_j, "cmd_data");
	ota_data_j = json_object_get(cmd_data_j, "ota");
	rc = serv_ota_obj_parse(ota_data_j, &ota_type, &checksum, &url, &ver,
	    &size);
	if (rc) {
		return -1;
	}
	save_location = json_get_string(gcmd->info_j, "save_location");
	if (!save_location) {
		return -1;
	}
	fd = open(save_location, O_RDONLY);
	if (fd == -1) {
		log_err("open failed for %s: err %m", save_location);
		return -1;
	}
	file_size = lseek(fd, 0, SEEK_END);
	if (file_size != size) {
		log_warn("bad node image, expected %zd, recvd %zd",
		    size, file_size);
checks_failed:
		close(fd);
		return -1;
	}
	lseek(fd, 0, SEEK_SET);
	if (gateway_calc_md5_of_file(fd, md5_read, sizeof(md5_read))) {
		goto checks_failed;
	}
	if (strcasecmp(md5_read, checksum)) {
		log_warn("corrupted node image, expected md5 %s, recvd %s",
		    checksum, md5_read);
		goto checks_failed;
	}
	close(fd);
	return 0;
}

/*
 * Get the (s3) location of a node OTA image.
 */
static int gateway_node_ota_url_fetch_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	json_t *cmd_data_j;
	json_t *ota_j;
	const char *url;

	cmd_data_j = json_object_get(gcmd->info_j, "cmd_data");
	ota_j = json_object_get(cmd_data_j, "ota");
	url = json_get_string(ota_j, "url");
	if (!url || *url == '\0') {
		log_warn("missing OTA info URL");
		return -1;
	}
	snprintf(link, link_size, "%s", url);
	*method = HTTP_GET;

	return 0;
}

/*
 * Successfully fetched the (ephemeral, i.e. s3) location of the OTA image.
 * Now go fetch and store it in a file.
 */
static int gateway_node_ota_url_fetch_success(struct ops_devd_cmd *op_cmd,
				    struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	json_t *ota_obj;

	if (!req_resp) {
		log_err("failed to parse response");
		return -1;
	}
	ota_obj = json_object_get(req_resp, "ota");
	if (!ota_obj) {
		log_warn("no ota object");
		return -1;
	}
	json_object_set(gcmd->info_j, "cmd_data", req_resp);
	/* fetch node ota from remote location */
	op_cmd->op_handlers = &gw_op_handlers[GCS_NODE_OTA_REMOTE_FETCH];

	return 1;
}

/*
 * Response to node ota command put succeeded. If save_location == NULL,
 * appd wants the OTA image discarded so just finish in that case. If
 * save_location != NULL, we need to go ahead and fetch the image from that
 * location.
 */
static int gateway_node_ota_success(struct ops_devd_cmd *op_cmd,
				    struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	json_t *cmd_data_j;
	json_t *ota_data_j;
	size_t size;
	const char *ota_type;
	const char *checksum;
	const char *url;
	const char *ver;
	json_t *save_location;
	const char *ota_source;
	int rc = -1;

	save_location = json_object_get(gcmd->info_j, "save_location");
	if (json_is_null(save_location)) {
		return 0;
	}
	cmd_data_j = json_object_get(gcmd->info_j, "cmd_data");
	ota_data_j = json_object_get(cmd_data_j, "ota");
	rc = serv_ota_obj_parse(ota_data_j, &ota_type, &checksum, &url, &ver,
	    &size);
	if (rc) {
		return -1;
	}
	ota_source = json_get_string(ota_data_j, "source");
	if (ota_source && strcmp(ota_source, "local")) {
		/* for non-local OTA, schedule GET for server info */
		op_cmd->op_handlers = &gw_op_handlers[GCS_NODE_OTA_URL_FETCH];
	} else {
		/* fetch node ota from local location */
		op_cmd->op_handlers = &gw_op_handlers[GCS_NODE_OTA_LOCAL_FETCH];
	}

	return 1;
}

/*
 * Add node succeeded
 */
static int gateway_node_add_success(struct ops_devd_cmd *op_cmd,
				    struct device_state *dev)
{
	const char *addr;
	const char *dsn;
	json_t *device_j;

	char oem_model[64] = {0};
	bool dsn_valid_flag = true;
	const char *oem;
	json_t *root;
	json_error_t error;
	json_t *node_obj;

	if(strlen(req_json_paylod_copy) > 0)
	{
		root = json_loads( req_json_paylod_copy, 0, &error );
		if ( !root )
		{
    			log_debug("error: on line %d: %s\n", error.line, error.text );
			return -1;
		}else{
			node_obj = json_object_get(root, "node");
			if (!node_obj) {
				log_warn("missing device object");
				return -1;
			}else{
				oem = json_get_string(node_obj, "oem_model");
				if(!oem)
				{
					log_debug("oem_model not avail");
					return -1;
				}
				else{
					memcpy(oem_model,oem,strlen(oem));
					log_info("parsing oem model is %s", oem_model);
				}
			}
			json_decref(root);
		}
	}

	if (!req_resp) {
		log_err("failed to parse response");
		return -1;
	}
	ds_json_dump(__func__, req_resp);
	device_j = json_object_get(req_resp, "device");
	if (!device_j) {
		log_warn("missing device object");
		return -1;
	}
	addr = json_get_string(device_j, "address");
	dsn = json_get_string(device_j, "dsn");
	
	if (!addr || !dsn) {
		log_warn("missing addr or dsn");
		return -1;
	}
	/*Validate DSN*/
	for(int i = 0; i <strlen(dsn); i++){
    		char ch = dsn[i];
    		if(ch == '\0')
		     break; //stop iterating at end of string

     		if((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
    		{
			dsn_valid_flag = true;
    		}
    		else if(ch >= '0' && ch <= '9')
    		{
			dsn_valid_flag = true;
    		}
    		else
    		{
        		log_debug("'%c' is special character.", ch);
			dsn_valid_flag = false;
			break;
    		}	
	}
	if(true == dsn_valid_flag)
	{
		if (gateway_mapping_add(dsn, addr) < 0) {
			log_err("add node failed: %s --> %s", dsn, addr);
			return -1;
		}
		log_debug("add node: %s --> %s", dsn, addr);
		if(0 == strcmp(oem_model,"virtualnode"))
		{
			InsertNode(addr);
			log_info("vtnode not needed to save in file");
		}else{
			conf_save();
		}
	}else{
		log_debug("invalid dsn recvd from cloud [%s]",dsn);
		return -1;
	}

	/* Start request for video streams (KVS & WebRTC) */
	if(start_video_stream_request(dev, addr) != 0) {
		log_err("start video stream request failed");
		return -1;
	}

	log_debug("add node: %s --> %s", dsn, addr);
	conf_save();
	
	return 0;
}

/*
 * Remove node succeeded.
 */
static int gateway_node_remove_success(struct ops_devd_cmd *op_cmd,
	struct device_state *dev)
{
	struct gateway_cmd *gcmd = NULL;
	const char *dsn;
	const char *addr;

	if (!op_cmd) {
		log_err("op_cmd NULL");
		return -1;
	}
	gcmd = (struct gateway_cmd *)(op_cmd->arg);
	if (!gcmd) {
		log_err("gcmd NULL");
		return -1;
	}

	ds_json_dump(__func__, gcmd->info_j);
	addr = json_get_string(gcmd->info_j, "address");
	if (!addr) {
		log_err("couldn't get node addr");
		return -1;
	}
	dsn = gateway_addr_to_dsn(addr);
	if (!dsn) {
		log_warn("couldn't find dsn for addr %s", addr);
		return -1;
	}
	log_debug("delete node: %s --> %s", dsn, addr);
	if (!gateway_mapping_delete(dsn, addr)) {
		RemoveNode(addr);
		conf_save();
	}
	return 0;
}

/*
 * Initialize an ADD node operation for gateways
 */
static int gateway_node_add_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	char *buf = NULL;
	json_t *obj = gcmd->info_j;

	if (!gcmd) {
		log_err("gcmd NULL");
		return -1;
	}

	snprintf(link, DS_CLIENT_LINK_MAX_LEN,
	    "https://%s/%s/dsns/%s/nodes.json",
	    dev->ads_host, ADS_API_VERSION, dev->dsn);
	log_debug("link %s", link);

	ds_client_data_init_json(&info->req_data, gcmd->info_j);

	/*Copy the node add init request json payload used to validate node is WiFi virtual node or Sensor node*/
	{
		if (!obj) {
			log_err("node add init NULL JSON object");
		}
		else{
			buf = json_dumps(obj, JSON_COMPACT);
			if (!buf) {
				log_err("node add init malloc failed");
				return -1;
			}else{
				log_debug("node add init req buf %s", buf);
				memset(req_json_paylod_copy,0x00,sizeof(req_json_paylod_copy));
				memcpy(req_json_paylod_copy,buf,strlen(buf));
				free(buf);
			}
		}
	}

	info->init = 1;
	*method = HTTP_POST;

	return 0;
}

/*
 * Initialize an UPDATE node operation for gateways
 */
static int gateway_node_update_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	const char *dsn;
	const char *address;

	if (!gcmd) {
		log_err("gcmd NULL");
		return -1;
	}

	/*
	 * checks for existence of node and address object has already been
	 * done in gateway_parse data
	 */
	address = json_get_string(json_object_get(gcmd->info_j, "node"),
	    "address");
	dsn = gateway_addr_to_dsn(address);
	if (!dsn) {
		log_warn("couldn't find dsn for address %s", address);
		return -1;
	}
	snprintf(link, DS_CLIENT_LINK_MAX_LEN,
	    "https://%s/%s/dsns/%s/nodes/%s.json",
	    dev->ads_host, ADS_API_VERSION, dev->dsn, dsn);

	ds_client_data_init_json(&info->req_data, gcmd->info_j);
	info->init = 1;
	*method = HTTP_PUT;

	return 0;
}

/*
 * Initialize an REMOVE node operation for gateways
 */
static int gateway_node_remove_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	const char *dsn;
	const char *address;

	if (!gcmd) {
		log_err("gcmd NULL");
		return -1;
	}

	address = json_get_string(gcmd->info_j, "address");
	if (!address) {
		log_warn("couldn't get address");
		return -1;
	}
	dsn = gateway_addr_to_dsn(address);
	if (!dsn) {
		log_warn("couldn't find dsn for address %s", address);
		return -1;
	}
	snprintf(link, DS_CLIENT_LINK_MAX_LEN,
	    "https://%s/%s/dsns/%s/nodes/%s.json?force_delete=true",
	    dev->ads_host, ADS_API_VERSION, dev->dsn, dsn);

	*method = HTTP_DELETE;

	return 0;
}

/*
 * Convert a property info structure from appd. Take the subdevice_key,
 * template_key and name, into a delimited prop name. Optionally a bkup_j
 * object can be given to backup the information.
 */
int gateway_prop_info_to_name(json_t *prop_info_j, json_t *bkup_j)
{
	const char *prop_name;
	const char *subdevice_key;
	const char *template_key;
	char concat_prop_name[MAX_PROP_NAME_LEN + 1];

	prop_name = json_get_string(prop_info_j, "name");
	subdevice_key = json_get_string(prop_info_j, "subdevice_key");
	template_key = json_get_string(prop_info_j, "template_key");
	if (!prop_name || !subdevice_key || !template_key) {
		return -1;
	}
	snprintf(concat_prop_name, sizeof(concat_prop_name),
	    CLOUD_PROP_NAME_FORMAT, subdevice_key, template_key, prop_name);
	if (bkup_j) {
		json_object_set_new(bkup_j, "name", json_string(prop_name));
		json_object_set_new(bkup_j, "subdevice_key",
		    json_string(subdevice_key));
		json_object_set_new(bkup_j, "template_key",
		    json_string(template_key));
	}
	json_object_del(prop_info_j, "subdevice_key");
	json_object_del(prop_info_j, "template_key");
	json_object_set_new(prop_info_j, "name", json_string(concat_prop_name));

	return 0;
}

/*
 * Helper function for sending prop updates/echoes to a LAN client
 */
static int gateway_prop_send_lan_init_helper(struct device_state *dev,
	struct prop_send_handlers *handler)
{
	struct ops_devd_cmd *op_cmd = handler->op_cmd;
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	json_t *prop_info_j;

	if (!gcmd) {
		return -1;
	}
	prop_info_j = json_object_get(gcmd->info_j, "property");
	if (!prop_info_j) {
		return -1;
	}
	if (gateway_convert_address_to_dsn(prop_info_j)) {
		return -1;
	}
	if (op_cmd->dests_target > 1) {
		/*
		 * Post to LAN clients first.
		 */
		prop_send_prop_to_lan_clients(op_cmd, dev, op_cmd->dests_target,
		    prop_info_j, LAN_NODE_PROP_SEND_URL);
	}

	return 0;
}

/*
 * Helper function for intializing a prop send/echo to ADS
 */
static void gateway_prop_send_ads_init_helper(struct device_state *dev,
	struct prop_send_handlers *handler)
{
	struct ops_devd_cmd *op_cmd = handler->op_cmd;
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	const char *node_dsn;
	const char *prop_name;
	json_t *prop_info_j;

	prop_info_j = json_object_get(gcmd->info_j, "property");
	prop_curl_buf_info_setup(handler->info, prop_info_j);
	*handler->method = HTTP_POST;

	node_dsn = json_get_string(prop_info_j, "dsn");
	prop_name = json_get_string(prop_info_j, "name");
	snprintf(handler->link, handler->link_size,
	    "https://%s/%s/dsns/%s/properties/%s/datapoints.json%s",
	    dev->ads_host, ADS_API_VERSION, node_dsn, prop_name,
	    op_cmd->echo ? "?echo=true" : "");
}

/*
 * Initialize a prop send/echo.
 * Returns 0 on success
 * Returns -1 on failure
 * Returns 1 if success but no operation needed on ADS
 */
static int gateway_prop_send_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_send_handlers handler = {
	    gateway_prop_send_lan_init_helper,
	    gateway_prop_send_ads_init_helper,
	    method,
	    link,
	    link_size,
	    info,
	    op_cmd
	};

	return prop_send_init_execute(&handler);
}

/*
 * Helper function for sending batch prop updates to a LAN client
 */
static int gateway_prop_batch_send_lan_init_helper(struct device_state *dev,
	struct prop_send_handlers *handler)
{
	struct ops_devd_cmd *op_cmd = handler->op_cmd;
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	json_t *prop_info_j;
	int i;
	u8 dests = op_cmd->dests_target;

	if (!gcmd) {
		return -1;
	}
	for (i = 0; i < json_array_size(gcmd->info_j); i++) {
		prop_info_j = json_object_get(json_array_get(gcmd->info_j, i),
		    "property");
		if (!prop_info_j) {
			log_warn("no prop obj found");
			return -1;
		}
		if (gateway_prop_info_to_name(prop_info_j, NULL)) {
			return -1;
		}
		if (gateway_convert_address_to_dsn(prop_info_j)) {
			return -1;
		}
	}
	if (dests & ~DEST_ADS) {
		prop_send_batch_to_lan_clients(op_cmd, dev, gcmd->info_j,
		    LAN_NODE_PROP_SEND_URL);
	}

	return 0;
}

/*
 * Helper function for intializing a batch prop send/echo to ADS
 */
static void gateway_prop_batch_send_ads_init_helper(struct device_state *dev,
	struct prop_send_handlers *handler)
{
	struct ops_devd_cmd *op_cmd = handler->op_cmd;
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;

	prop_batch_payload_construct(dev, handler, BATCH_NODE_DPS_URL,
	    gcmd->info_j);
}

/*
 * Initialize a batch datapoint send
 * Returns 0 on success
 * Returns -1 on failure
 * Returns 1 if success but no operation needed on ADS
 */
static int gateway_prop_batch_send_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct prop_send_handlers handler = {
	    gateway_prop_batch_send_lan_init_helper,
	    gateway_prop_batch_send_ads_init_helper,
	    method,
	    link,
	    link_size,
	    info,
	    op_cmd
	};

	return prop_send_init_execute(&handler);
}

/*
 * Acknowledgment a node property datapoint
 */
static int gateway_prop_ack_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	json_t *prop_info_j = json_object_get(gcmd->info_j, "property");
	const char *node_dsn;
	struct prop_send_handlers handlers = {
	    gateway_prop_send_lan_init_helper,
	    gateway_prop_send_ads_init_helper,
	    method,
	    link,
	    link_size,
	    info,
	    op_cmd,
	    prop_info_j
	};

	if (!prop_info_j) {
		return -1;
	}
	if (gateway_convert_address_to_dsn(prop_info_j)) {
		return -1;
	}
	node_dsn = json_get_string(prop_info_j, "dsn");
	return prop_ack_init_execute(&handlers, LAN_NODE_PROP_ACK_URL,
	    node_dsn);
}

/*
 * GET commands.json?(all=true | input=true) succeeded
 */
static int gateway_multiple_props_get_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	json_t *commands;

	if (!req_resp) {
		return -1;
	}
	commands = json_object_get(req_resp, "commands");
	if (!json_is_object(commands)) {
		return -1;
	}
	ds_parse_props(dev, commands, NULL, gateway_send_prop_resp, op_cmd);
	dev->par_content = (req_status == HTTP_STATUS_PAR_CONTENT);
	return 0;
}

/*
 * Get props ?input=true and ?all=true
 * Returns 0 on success
 */
static int gateway_multiple_props_get_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	const char *addr = json_get_string(gcmd->info_j, "address");
	const char *dsn;

	dsn = gateway_addr_to_dsn(addr);
	if (!dsn) {
		return -1;
	}
	snprintf(link, link_size,
	    "https://%s/%s/dsns/%s/commands.json?%s",
	    dev->ads_host, ADS_API_VERSION, dsn,
	    (gcmd->op == AG_PROP_REQ_ALL) ? "all=true" : "input=true");

	*method = HTTP_GET;

	return 0;
}

/*
 * Prop get succeeded
 * Handle incoming response to GET property/<prop_name>.json
 * Examples:
 * "property": {
 *      "base_type": "boolean",
 *      "data_updated_at": "2014-02-04T18:38:36Z",
 *      "device_key": 2981,
 *      "direction": "input",
 *      "display_name": "Blue_LED",
 *      "key": 34973,
 *      "name": "Blue_LED",
 *      "product_name": "LinuxDev",
 *      "read_only": false,
 *      "scope": "user",
 *      "track_only_changes": false,
 *      "value": 1
 *  }
 *
 */
static int gateway_prop_get_success(struct ops_devd_cmd *op_cmd,
			    struct device_state *dev)
{
	return prop_get_success_helper(op_cmd, dev, gateway_send_prop_resp);
}

/*
 * Initialize a node prop get.
 */
static int gateway_prop_get_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	json_t *prop_info_j = json_object_get(gcmd->info_j, "property");
	const char *node_dsn;
	const char *prop_name;
	json_t *prop_j;

	if (!prop_info_j) {
		return -1;
	}
	if (gateway_convert_address_to_dsn(prop_info_j)) {
		return -1;
	}
	prop_j = json_object_get(gcmd->info_j, "property");
	node_dsn = json_get_string(prop_j, "dsn");
	prop_name = json_get_string(prop_j, "name");

	snprintf(link, link_size,
	    "https://%s/%s/dsns/%s/properties/%s.json",
	    dev->ads_host, ADS_API_VERSION, node_dsn, prop_name);

	*method = HTTP_GET;

	return 0;
}

/*
 * Send appd's response to the node factory reset command
 */
static int gateway_node_rst_result_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	int success;
	int msg_code;
	int cmd_id;
	const char *cmd_uri;

	if (json_get_int(gcmd->info_j, "success", &success)) {
err:
		return -1;
	}
	if (json_get_int(gcmd->info_j, "msg_code", &msg_code)) {
		goto err;
	}
	if (json_get_int(gcmd->info_j, "cmd_id", &cmd_id)) {
		goto err;
	}
	cmd_uri = json_get_string(gcmd->info_j, "cmd_uri");
	if (!cmd_uri) {
		goto err;
	}
	snprintf(link, link_size,
	    "https://%s/devices/%s%s?cmd_id=%d&status=%d&msg_code=%d",
	    dev->ads_host, dev->key, cmd_uri, cmd_id,
	    success ? HTTP_STATUS_OK : HTTP_STATUS_NOT_ACCEPT, msg_code);
	*method = HTTP_PUT;

	return 0;
}

/*
 * Fetch OTA image from a URL
 */
static int gateway_node_ota_fetch_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	const char *value;
	json_t *cmd_data_j;
	json_t *ota_data_j;

	value = json_get_string(gcmd->info_j, "save_location");
	if (!value) {
		return -1;
	}
	cmd_data_j = json_object_get(gcmd->info_j, "cmd_data");
	ota_data_j = json_object_get(cmd_data_j, "ota");

	if (json_get_string_copy(ota_data_j, "url", link, link_size) < 0) {
		return -1;
	}
	if (op_cmd->op_handlers->op_name ==
	    &gateway_ops[GCS_NODE_OTA_REMOTE_FETCH]) {
		info->non_ayla = 1;
	}
	info->resp_file_path = value;
	info->init = 1;
	*method = HTTP_GET;

	return 0;
}

/*
 * Send appd's response to the node ota command. PUT for the OTA reverse-rest
 * command.
 */
static int gateway_node_ota_init(enum http_method *method, char *link,
			int link_size, struct ops_buf_info *info,
			struct ops_devd_cmd *op_cmd, struct device_state *dev)
{
	struct gateway_cmd *gcmd = (struct gateway_cmd *)op_cmd->arg;
	json_t *save_location;
	json_t *ota_data_j;
	int cmd_id;
	const char *cmd_uri;

	if (json_get_int(gcmd->info_j, "cmd_id", &cmd_id) < 0) {
err:
		return -1;
	}
	cmd_uri = json_get_string(gcmd->info_j, "cmd_uri");
	if (!cmd_uri) {
		goto err;
	}
	ota_data_j = json_object_get(gcmd->info_j, "cmd_data");
	if (!ota_data_j) {
		goto err;
	}
	save_location = json_object_get(gcmd->info_j, "save_location");
	snprintf(link, link_size,
	    "https://%s/devices/%s%s?cmd_id=%d&status=%d",
	    dev->ads_host, dev->key, cmd_uri, cmd_id,
	    json_is_null(save_location) ? HTTP_STATUS_NOT_ACCEPT :
	    HTTP_STATUS_OK);
	*method = HTTP_PUT;

	return 0;
}

/*
 * Setup an internal gw cmd to echo node property updates
 * if other destinations exist
 */
void gateway_node_prop_prepare_echo(struct device_state *dev, json_t *elem_j,
	int source)
{
	struct ops_devd_cmd *op_cmd;
	struct gateway_cmd *gcmd;
	const char *prop_name;
	const char *err_name;
	json_t *prop_info_j;
	json_t *opargs_j;
	u8 echo_dest = dev->dests_avail & ~SOURCE_TO_DEST_MASK(source);

	if (!echo_dest) {
		return;
	}
	opargs_j = json_object();
	prop_info_j = json_object_get(elem_j, "property");
	if (gateway_prop_info_to_name(prop_info_j, opargs_j)) {
		json_decref(opargs_j);
		return;
	}
	if (debug) {
		prop_name = json_get_string(prop_info_j, "name");
		log_debug("echoing %s to %d", prop_name, echo_dest);
		ds_json_dump(__func__, prop_info_j);
	}
	gcmd = calloc(1, sizeof(*gcmd));
	REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
	gcmd->info_j = json_incref(elem_j);
	op_cmd = calloc(1, sizeof(*op_cmd));
	REQUIRE(op_cmd, REQUIRE_MSG_ALLOCATION);
	op_cmd->proto = JINT_PROTO_GATEWAY;
	op_cmd->dests_target = echo_dest;
	op_cmd->echo = 1;
	op_cmd->arg = gcmd;
	err_name = json_get_string(prop_info_j, "address");
	if (err_name) {
		op_cmd->err_name = strdup(err_name);
		json_object_set(opargs_j, "address",
		    json_object_get(prop_info_j, "address"));
	}
	op_cmd->op_handlers = &gw_op_handlers[GCS_AUTO_ECHO];
	op_cmd->op_args = json_array();
	json_array_append_new(op_cmd->op_args, opargs_j);
	ops_devd_add(op_cmd);
}

/*
 * Handle a gateway operation
 */
static enum app_parse_rc gateway_parse_data(json_t *cmd, int recv_id,
					  json_t *opts)
{
	const char *opstr = json_get_string(cmd, "op");
	struct device_state *dev = &device;
	enum ayla_gateway_op op;
	struct ops_devd_cmd *op_cmd;
	struct gateway_cmd *gcmd = NULL;
	json_t *args;
	u8 confirm;
	u8 echo;
	u8 dests = DEST_ADS;
	int source = 0;
	const char *err_name;
	enum gateway_client_state gcs = GCS_NOP;
	json_t *node_j;
	json_t *prop_info_j;
	json_t *opargs_j = NULL;
	u8 dests_specified = 0;

	if (!opstr) {
		gateway_send_nak(JINT_ERR_OP, recv_id);
		return APR_DONE;
	}
	op = gateway_op_get(opstr);
	confirm = json_is_true(json_object_get(opts, "confirm"));
	echo = json_is_true(json_object_get(opts, "echo"));
	args = json_object_get(cmd, "args");
	if (!json_is_array(args)) {
err:
		gateway_send_nak(JINT_ERR_INVAL_ARGS, recv_id);
		if (gcmd) {
			gateway_free_gateway_cmd(gcmd);
		}
		if (confirm) {
			jint_send_confirm_false(JINT_PROTO_GATEWAY,
			    recv_id, dests, JINT_ERR_INVAL_ARGS);
		}
		return APR_ERR;
	}
	switch (op) {
	case AG_NODE_ADD:
	case AG_NODE_UPDATE:
		if (json_array_size(args) == 0) {
			return APR_DONE;
		}
		gcmd = calloc(1, sizeof(*gcmd));
		REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
		gcmd->info_j = json_incref(json_array_get(args, 0));
		node_j = json_object_get(gcmd->info_j, "node");
		if (!node_j) {
			goto err;
		}
		err_name = json_get_string(node_j, "address");
		if (!err_name) {
			goto err;
		}
		echo = 0;	/* gateway node add/updates cannot be echoes */
		gateway_send_ack(recv_id);
		gcs = (op == AG_NODE_ADD) ? GCS_NODE_ADD : GCS_NODE_UPDATE;
		log_debug("address %s, op %d, AG_NODE_ADD %d",
		    err_name, op, AG_NODE_ADD);
		break;
	case AG_NODE_REMOVE:
		if (json_array_size(args) == 0) {
			return APR_DONE;
		}
		gcmd = calloc(1, sizeof(*gcmd));
		REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
		gcmd->info_j = json_incref(json_array_get(args, 0));
		err_name = json_get_string(gcmd->info_j, "address");
		gateway_send_ack(recv_id);
		gcs = GCS_NODE_REMOVE;
		break;
	case AG_CONN_STATUS:
		if (json_array_size(args) == 0) {
			return APR_DONE;
		}
		gcmd = calloc(1, sizeof(*gcmd));
		REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
		gcmd->info_j = json_incref(args);
		err_name = json_get_string(json_array_get(args, 0), "address");
		gateway_send_ack(recv_id);
		if (dev->dests_avail) {
			dests = dev->dests_avail;
		}
		gcs = GCS_CONN_STATUS;
		break;
	case AG_PROP_SEND:
	case AG_PROP_REQ:
	case AG_PROP_ACK:
		if (json_array_size(args) == 0) {
			return APR_DONE;
		}
		gcmd = calloc(1, sizeof(*gcmd));
		REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
		gcmd->info_j = json_incref(json_array_get(args, 0));
		prop_info_j = json_object_get(gcmd->info_j, "property");
		if (!prop_info_j) {
			goto err;
		}
		opargs_j = json_object();
		REQUIRE(opargs_j, REQUIRE_MSG_ALLOCATION);
		if (gateway_prop_info_to_name(prop_info_j, opargs_j)) {
			json_decref(opargs_j);
			goto err;
		}
		err_name = json_get_string(prop_info_j, "address");
		if (err_name) {
			json_object_set(opargs_j, "address",
			    json_object_get(prop_info_j, "address"));
		}
		switch (op) {
		case AG_PROP_SEND:
			dests = prop_get_dests_helper(dev, opts,
			    &dests_specified);

			/* Execute special action for KVS & WebRTC video stream update */
			const char* prop_name = json_get_string(prop_info_j, "name");
			const char* base_type = json_get_string(prop_info_j, "base_type");
			int val;
			if(strcmp(prop_name, "s1:kvs_base:kvs_stream_update") == 0 &&
				strcmp(base_type, "integer") == 0)
			{
				json_get_int(prop_info_j, "value", &val);
				if(1 == val) {
					if (ds_update_kvs_streaming_channel(err_name) != 0) {
						log_err("Failed to update KVS streaming channel");
					}
				}
			}
			else if(strcmp(prop_name, "s1:kvs_base:webrtc_stream_update") == 0 &&
					strcmp(base_type, "integer") == 0)
			{
				json_get_int(prop_info_j, "value", &val);
				if(1 == val) {
					if (ds_update_webrtc_streaming_channel(err_name) != 0) {
						log_err("Failed to update WebRTC streaming channel");
					}
				}
			}

			gateway_send_ack(recv_id);
			gcs = GCS_PROP_SEND;
			break;
		case AG_PROP_REQ:
			gateway_send_ack(recv_id);
			gcs = GCS_GET_PROP;
			break;
		case AG_PROP_ACK:
			json_get_int(opts, "source", &source);
			if (!source) {
				goto err;
			}
			dests = dev->dests_avail | SOURCE_TO_DEST_MASK(source);
			gateway_send_ack(recv_id);
			gcs = GCS_PUT_ACK;
			break;
		default:
			goto err;
		}
		break;
	case AG_PROP_BATCH_SEND:
		if (json_array_size(args) == 0) {
			return APR_DONE;
		}
		gcmd = calloc(1, sizeof(*gcmd));
		REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
		gcmd->info_j = json_incref(args);
		err_name = gateway_ops[AG_PROP_BATCH_SEND];
		dests = prop_get_dests_helper(dev, opts, &dests_specified);
		gateway_send_ack(recv_id);
		gcs = GCS_PROP_BATCH_SEND;
		break;
	case AG_CONN_STATUS_RESP:
		gateway_handle_conn_status_resp(cmd, recv_id);
		return APR_DONE;
	case AG_PROP_RESP:
		gateway_handle_property_resp(cmd, recv_id);
		return APR_DONE;
	case AG_NODE_RST_RESULT:
	case AG_NODE_OTA_RESULT:
		if (json_array_size(args) == 0) {
			return APR_DONE;
		}
		gcmd = calloc(1, sizeof(*gcmd));
		REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
		gcmd->info_j = json_incref(json_array_get(args, 0));
		err_name = json_get_string(gcmd->info_j, "address");
		echo = 0;
		dests = DEST_ADS;
		gateway_send_ack(recv_id);
		if (op == AG_NODE_RST_RESULT) {
			gcs = GCS_NODE_RST_RESULT;
		} else {
			/* node ota */
			gcs = GCS_NODE_OTA_RESULT;
		}
		break;
	case AG_NODE_REG_RESULT:
		if (json_array_size(args) == 0) {
			return APR_DONE;
		}
		gcmd = calloc(1, sizeof(*gcmd));
		REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
		gcmd->info_j = json_incref(json_array_get(args, 0));
		err_name = json_get_string(gcmd->info_j, "address");
		echo = 0;
		dests = DEST_ADS;
		gateway_send_ack(recv_id);
		gcs = GCS_NODE_REG_RESULT;
		break;
	case AG_PROP_REQ_ALL:
	case AG_PROP_REQ_TO_DEV:
		if (json_array_size(args) == 0) {
			return APR_DONE;
		}
		gcmd = calloc(1, sizeof(*gcmd));
		REQUIRE(gcmd, REQUIRE_MSG_ALLOCATION);
		gcmd->info_j = json_incref(json_array_get(args, 0));
		err_name = json_get_string(gcmd->info_j, "address");
		if (!err_name) {
			goto err;
		}
		if (op == AG_PROP_REQ_ALL) {
			gcs = GCS_GET_ALL_PROPS;
		} else {
			gcs = GCS_GET_TODEV_PROPS;
		}
		break;
	default:
		log_err("can't process opcode %d", op);
		return APR_ERR;
	}
	op_cmd = calloc(1, sizeof(*op_cmd));
	REQUIRE(op_cmd, REQUIRE_MSG_ALLOCATION);
	op_cmd->proto = JINT_PROTO_GATEWAY;
	op_cmd->req_id = recv_id;
	op_cmd->confirm = confirm;
	op_cmd->source = source;
	op_cmd->echo = echo;
	op_cmd->arg = gcmd;
	op_cmd->dests_specified = dests_specified;
	if (err_name) {
		op_cmd->err_name = strdup(err_name);
	}
	op_cmd->dests_target = dests;
	if (opargs_j) {
		op_cmd->op_args = json_array();
		json_array_append_new(op_cmd->op_args, opargs_j);
	}
	op_cmd->op_handlers = &gw_op_handlers[gcs];
	ops_devd_add(op_cmd);

	return APR_DONE;
}

/*
 * Load gateway client config.
 */
static int gateway_conf_set(json_t *obj)
{
	json_t *mappings_arr;
	json_t *map_obj;
	const char *dsn;
	const char *addr;
	int i;

	mappings_arr = json_object_get(obj, "node_mappings");
	if (!json_is_array(mappings_arr)) {
		return 0;
	}
	json_array_foreach(mappings_arr, i, map_obj) {
		dsn = json_get_string(map_obj, "dsn");
		addr = json_get_string(map_obj, "address");
		if (!dsn || !addr) {
			log_warn("bad gateway config");
			continue;
		}
		if (gateway_mapping_add(dsn, addr) < 0) {
			log_err("load node failed: %s --> %s", dsn, addr);
		} else {
			log_debug("load node: %s --> %s", dsn, addr);
		}
	}
	return 0;
}

/*
 * Save gateway client config
 */
static json_t *gateway_conf_get(void)
{
	json_t *gateway_obj;
	json_t *mappings_arr;
	json_t *obj;
	struct hashmap_iter *iter;
	const char *dsn;
	const char *addr;

	gateway_obj = json_object();
	mappings_arr = json_array();
	json_object_set_new(gateway_obj, "node_mappings", mappings_arr);

	for (iter = hashmap_iter(&gw_dsn_to_addr); iter;
	    iter = hashmap_iter_next(&gw_dsn_to_addr, iter)) {
		dsn = gateway_hashmap_iter_get_key(iter);
		addr = gateway_hashmap_iter_get_data(iter);
		if(0 == findNode(addr))
		{
			obj = json_object();
			json_array_append_new(mappings_arr, obj);
			json_object_set_new(obj, "dsn", json_string(dsn));
			json_object_set_new(obj, "address", json_string(addr));
		}else{
			log_info("%d vt_node dsn info not added into file",__LINE__);
		}
	}
	return gateway_obj;
}

/*
 * Initialize the gateway subsystem.
 */
int gateway_init(void)
{
	log_set_subsystem(LOG_SUB_GATEWAY);

	if (gateway_hash_init() < 0) {
		log_err("failed to initialize hash tables");
		return -1;
	}

	app_register_gateway_handler(gateway_parse_data);

	return conf_register("gateway", gateway_conf_set, gateway_conf_get);
}


static const struct op_funcs gw_op_handlers[] = {
	[GCS_NOP] = {NULL, gateway_nop_init},
	[GCS_NODE_ADD] = {&gateway_ops[AG_NODE_ADD],
	    gateway_node_add_init, gateway_node_add_success,
	    gateway_generic_op_finished},
	[GCS_NODE_UPDATE] = {&gateway_ops[AG_NODE_UPDATE],
	    gateway_node_update_init, NULL, gateway_generic_op_finished},
	[GCS_NODE_REMOVE] = {&gateway_ops[AG_NODE_REMOVE],
	    gateway_node_remove_init, gateway_node_remove_success,
	    gateway_generic_op_finished},
	[GCS_CONN_STATUS] = {&gateway_ops[AG_CONN_STATUS],
	    gateway_conn_status_init, NULL, gateway_generic_op_finished},
	[GCS_PROP_SEND] = {&gateway_ops[AG_PROP_SEND], gateway_prop_send_init,
	    NULL, gateway_generic_op_finished},
	[GCS_PROP_BATCH_SEND] = {&gateway_ops[AG_PROP_BATCH_SEND],
	    gateway_prop_batch_send_init,
	    prop_batch_post_success, gateway_generic_op_finished},
	[GCS_GET_PROP] = {&gateway_ops[AG_PROP_REQ], gateway_prop_get_init,
	    gateway_prop_get_success, gateway_generic_op_finished},
	[GCS_GET_ALL_PROPS] = {&gateway_ops[AG_PROP_REQ_ALL],
	    gateway_multiple_props_get_init, gateway_multiple_props_get_success,
	    gateway_generic_op_finished},
	[GCS_GET_TODEV_PROPS] = {&gateway_ops[AG_PROP_REQ_TO_DEV],
	    gateway_multiple_props_get_init, gateway_multiple_props_get_success,
	    gateway_generic_op_finished},
	[GCS_AUTO_ECHO] = {&gateway_ops[AG_ECHO_FAILURE],
	    gateway_prop_send_init, NULL, gateway_generic_op_finished},
	[GCS_NODE_RST_RESULT] = {&gateway_ops[AG_NODE_RST_RESULT],
	    gateway_node_rst_result_init, gateway_node_rst_success,
	    gateway_generic_op_finished},
	[GCS_NODE_OTA_RESULT] = {&gateway_ops[AG_NODE_OTA_RESULT],
	    gateway_node_ota_init, gateway_node_ota_success,
	    gateway_generic_op_finished},
	[GCS_NODE_OTA_URL_FETCH] = {&gateway_ops[AG_NODE_OTA_URL_FETCH],
	    gateway_node_ota_url_fetch_init, gateway_node_ota_url_fetch_success,
	    gateway_generic_op_finished},
	[GCS_NODE_OTA_LOCAL_FETCH] = {&gateway_ops[AG_NODE_OTA_LOCAL_FETCH],
	    gateway_node_ota_fetch_init, gateway_node_ota_fetch_success,
	    gateway_generic_op_finished},
	[GCS_NODE_OTA_REMOTE_FETCH] = {&gateway_ops[AG_NODE_OTA_REMOTE_FETCH],
	    gateway_node_ota_fetch_init, gateway_node_ota_fetch_success,
	    gateway_generic_op_finished},
	[GCS_PUT_ACK] = {&gateway_ops[AG_PROP_ACK],
	    gateway_prop_ack_init, NULL, gateway_generic_op_finished},
	[GCS_NODE_REG_RESULT] = {&gateway_ops[AG_NODE_REG_RESULT],
	    gateway_node_rst_result_init, NULL, gateway_generic_op_finished},
};

void gateway_video_stream_request_statemachine(struct timer* timer)
{
	struct device_state *dev = (struct device_state*)timer->data;
	struct video_stream_request* req = &dev->video_stream_req;

	if(! req->request) {
		return;
	}

	log_debug("video stream request statemachine step: %d", req->step);
	switch(req->step)
	{
		case DS_VIDEO_STREAM_REQUEST_STEP_IDLE:
		{
			req->step = DS_VIDEO_STREAM_REQUEST_STEP_KVS;
			timer_set(&dev->timers, &req->timer, 500);

			break;
		}
		case DS_VIDEO_STREAM_REQUEST_STEP_KVS:
		{
			/* Request KVS stream info */
			log_debug("calling the kvs streaming channel get ******************* ");
			if (ds_get_kvs_streaming_channel(dev, req->addr_curr) < 0) {
				ds_cloud_failure(0);
			}

			req->step = DS_VIDEO_STREAM_REQUEST_STEP_WEBRTC;
			timer_set(&dev->timers, &req->timer, req->timeout_ms);

			break;
		}
		case DS_VIDEO_STREAM_REQUEST_STEP_WEBRTC:
		{
			/* Request WebRTC stream info */
			log_debug("calling the webrtc signalling channel get ******************* ");
			if (ds_get_webrtc_signalling_channel(dev, req->addr_curr) < 0) {
				ds_cloud_failure(0);
			}

			req->step = DS_VIDEO_STREAM_REQUEST_STEP_DONE;
			timer_set(&dev->timers, &req->timer, req->timeout_ms);

			break;
		}
		case DS_VIDEO_STREAM_REQUEST_STEP_DONE:
		{
			log_debug("video stream request finished");
			timer_cancel(&dev->timers, &req->timer);
			req->step = DS_VIDEO_STREAM_REQUEST_STEP_IDLE;
			free(req->addr_curr);
			req->addr_curr = NULL;
			req->request = false;

			break;
		}
		default:
		{
			log_err("unknown video stream request step %d", req->step);
			req->step = DS_VIDEO_STREAM_REQUEST_STEP_IDLE;
			req->request = false;
			req->addr_curr = NULL;
		}
	}
}

int start_video_stream_request(struct device_state *dev, const char* addr)
{
	if(dev->video_stream_req.request) {
		log_err("video stream request already in progress");
		return -1;
	}

	dev->video_stream_req.request = true;
	dev->video_stream_req.step = DS_VIDEO_STREAM_REQUEST_STEP_IDLE;
	dev->video_stream_req.addr_curr = strdup(addr);
	timer_set(&dev->timers, &dev->video_stream_req.timer, dev->video_stream_req.timeout_ms);

	log_debug("video stream request started for ADDR: %s", addr);

	return 0;
}
