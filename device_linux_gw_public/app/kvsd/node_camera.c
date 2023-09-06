/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include <jansson.h>

#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <ayla/timer.h>
#include <ayla/json_parser.h>
#include <ayla/ayla_interface.h>

#include <app/app.h>
#include <app/props.h>
#include <app/gateway.h>
#include <sys/socket.h>

#include "gateway.h"
#include "node.h"
#include "node_camera.h"
#include "utils.h"
#include "video_stream.h"
#include "check.h"
#include "stream_comm.h"


/* Node details */
#define CAM_NODE_OEM_MODEL_PREFIX	"ggdemo_"
#define CAM_NODE_VERSION		"1.0"

/* Single subdevice supported */
 #define CAM_NODE_SUBDEVICE		"s1"

/* Template keys */
#define CAM_NODE_TEMPLATE_BASE		"kvs_base"
#define CAM_NODE_TEMPLATE_NODE		"kvs_cam"

/* Default between node property updates */
 #define CAM_NODE_SAMPLE_TIME_DEFAULT_MS	1000

#define CAM_URL_DEFAULT             		""
#define CAM_USERID_DEFAULT          		""
#define CAM_PASSWORD_DEFAULT        		""
#define CAM_MODEL_DEFAULT           		""
#define CAM_RES_WIDTH_DEFAULT       		0
#define CAM_RES_HEIGHT_DEFAULT      		0
#define CAM_FLIP_DEFAULT					0
#define CAM_BITRATE_MAX_DEFAULT     		0
#define CAM_STREAM_TIME_DEFAULT     		(-1)
#define CAM_STORAGE_SIZE_DEFAULT    		16
#define CAM_KVS_ENABLE_DEFAULT      		false
#define CAM_WEBRTC_ENABLE_DEFAULT   		false
#define CAM_KVS_STREAM_UPDATE_DEFAULT		0
#define CAM_WEBRTC_STREAM_UPDATE_DEFAULT	0

#define CAM_PROP_NAME_KVS_ENABLE            "kvs_enable"
#define CAM_PROP_NAME_WEBRTC_ENABLE         "webrtc_enable"
#define CAM_PROP_NAME_STREAM_TIME			"stream_time"
#define CAM_PROP_NAME_STORAGE_SIZE			"storage_size"

#define CAM_PROP_NAME_URL                   "url"
#define CAM_PROP_NAME_USERID                "user"
#define CAM_PROP_NAME_PASSWORD              "password"
#define CAM_PROP_NAME_MODEL                 "model"
#define CAM_PROP_NAME_RES_WIDTH             "width"
#define CAM_PROP_NAME_RES_HEIGHT            "height"
#define CAM_PROP_NAME_MAXBITRATE            "bitrate_max"
#define CAM_PROP_NAME_FLIP					"flip"
#define CAM_PROP_NAME_KVS_STREAM_UPDATE		"kvs_stream_update"
#define CAM_PROP_NAME_WEBRTC_STREAM_UPDATE	"webrtc_stream_update"

#define STREAM_START_DELAY_MS				10000	/* Delay before starting stream */

#define CAM_STEP_TIME_MS					15000	/* Update period */
#define CAM_STREAM_UPDATE_TIME_MS			(15 * 60 * 1000)	/* Stream update period */
#define CAM_STREAM_TIME_DIFF_THRESHOLD 		(2 * 60 * 60)	/* hours to seconds */

#define STREAM_BASE_PORT					5000

/*
 * Camera manager state
 */
struct camera_state {
	struct file_event_table *file_events;
	struct timer_head *timers;

	u32 node_index;			/* Index for new node address */

	struct timer step_timer;	/* Timer for global cam updates */
};

static struct camera_state state;

DEF_NAME_TABLE(cam_node_type_names, CAMERA_NODE_TYPES);

/*
 * Forward declarations
 */
static void cam_node_sample_timeout(struct timer *timer);

static void cam_node_prop_init_kvs_enable(struct node *node,
										  struct node_prop *prop);
static int cam_node_prop_update_kvs_enable(struct node *node,
										  struct node_prop *prop);

static void cam_node_prop_init_webrtc_enable(struct node *node,
										  struct node_prop *prop);
static int cam_node_prop_update_webrtc_enable(struct node *node,
										   struct node_prop *prop);

static void cam_node_prop_init_url_setpoint(struct node *node,
                                                  struct node_prop *prop);
static int cam_node_prop_update_url(struct node *node,
                                          struct node_prop *prop);

static void cam_node_prop_init_userid_setpoint(struct node *node,
                                                  struct node_prop *prop);
static int cam_node_prop_update_userid(struct node *node,
                                          struct node_prop *prop);

static void cam_node_prop_init_password_setpoint(struct node *node,
                                                  struct node_prop *prop);
static int cam_node_prop_update_password(struct node *node,
                                          struct node_prop *prop);

static void cam_node_prop_init_model_setpoint(struct node *node,
                                                  struct node_prop *prop);
static int cam_node_prop_update_model(struct node *node,
                                          struct node_prop *prop);

static void cam_node_prop_init_res_height_setpoint(struct node *node,
                                              struct node_prop *prop);
static int cam_node_prop_update_res_height(struct node *node,
                                      struct node_prop *prop);

static void cam_node_prop_init_res_width_setpoint(struct node *node,
											struct node_prop *prop);
static int cam_node_prop_update_res_width(struct node *node,
									struct node_prop *prop);

static void cam_node_prop_init_bitratemax_setpoint(struct node *node,
                                              struct node_prop *prop);
static int cam_node_prop_update_bitratemax(struct node *node,
                                      struct node_prop *prop);

static void cam_node_prop_init_stream_time_setpoint(struct node *node,
												   struct node_prop *prop);
static int cam_node_prop_update_stream_time(struct node *node,
										   struct node_prop *prop);

static void cam_node_prop_init_storage_size_setpoint(struct node *node,
													struct node_prop *prop);
static int cam_node_prop_update_storage_size(struct node *node,
											struct node_prop *prop);

static void cam_node_prop_init_flip_setpoint(struct node *node,
													 struct node_prop *prop);
static int cam_node_prop_update_flip(struct node *node,
											 struct node_prop *prop);

static void cam_node_prop_init_kvs_stream_update_setpoint(struct node *node,
											 struct node_prop *prop);
static int cam_node_prop_update_kvs_stream_update(struct node *node,
									 struct node_prop *prop);

static void cam_node_prop_init_webrtc_stream_update_setpoint(struct node *node,
														  struct node_prop *prop);
static int cam_node_prop_update_webrtc_stream_update(struct node *node,
												  struct node_prop *prop);

static void fork_and_start_kvs_streaming(struct node *node);
static void kvs_streaming_timeout(struct timer *timer);
static void kill_kvs_streaming(struct node* node);
static void kvs_streaming_start_delay_timeout(struct timer *timer);
static void webrtc_streaming_start_delay_timeout(struct timer *timer);
static void start_kvs_streaming(struct node* node);
static void kill_webrtc_streaming(struct node* node);
static void cam_kvs_stream_update_timeout(struct timer *timer);
static void cam_webrtc_stream_update_timeout(struct timer *timer);
static void start_webrtc_streaming(struct node* node);

static void start_master_stream(struct node* node);
static void get_master_stream_sock_path(char* path, size_t path_size, struct node* node);
static u16 get_addr_val(struct node* node);
static u16 get_hls_port(struct node* node);
static u16 get_webrtc_port(struct node* node);
static int master_stream_send_cmd(enum stream_comm_to_master_cmds cmd, struct cam_node_state* cam_node);
static void kill_master_stream(struct node* node);
static void cam_webrtc_stream_update_exec(struct cam_node_state *cam_node);
static void cam_kvs_stream_update_exec(struct cam_node_state *cam_node);


/*****************************************
 * Node template definition
 *****************************************/

struct cam_node_prop_def {
	struct node_prop_def def;
	void (*init)(struct node *, struct node_prop *);
	int (*set_callback)(struct node *, struct node_prop *);
};

static const struct cam_node_prop_def cam_template_base[] = {
	{{CAM_PROP_NAME_KVS_ENABLE, PROP_BOOLEAN, PROP_TO_DEVICE },
	 	cam_node_prop_init_kvs_enable, cam_node_prop_update_kvs_enable },
	{{CAM_PROP_NAME_WEBRTC_ENABLE, PROP_BOOLEAN, PROP_TO_DEVICE },
		cam_node_prop_init_webrtc_enable, cam_node_prop_update_webrtc_enable },
	{{CAM_PROP_NAME_STREAM_TIME, PROP_INTEGER, PROP_TO_DEVICE },
		cam_node_prop_init_stream_time_setpoint, cam_node_prop_update_stream_time },
	{{CAM_PROP_NAME_STORAGE_SIZE, PROP_INTEGER, PROP_TO_DEVICE },
		cam_node_prop_init_storage_size_setpoint, cam_node_prop_update_storage_size },
	{{CAM_PROP_NAME_KVS_STREAM_UPDATE, PROP_INTEGER, PROP_TO_DEVICE },
		cam_node_prop_init_kvs_stream_update_setpoint, cam_node_prop_update_kvs_stream_update },
	{{CAM_PROP_NAME_WEBRTC_STREAM_UPDATE, PROP_INTEGER, PROP_TO_DEVICE },
		cam_node_prop_init_webrtc_stream_update_setpoint, cam_node_prop_update_webrtc_stream_update },
};

static const struct cam_node_prop_def cam_template_node[] = {
    { { CAM_PROP_NAME_URL,	PROP_STRING,	PROP_TO_DEVICE },
            cam_node_prop_init_url_setpoint, cam_node_prop_update_url },
    { { CAM_PROP_NAME_USERID,	PROP_STRING,	PROP_TO_DEVICE },
            cam_node_prop_init_userid_setpoint, cam_node_prop_update_userid },
    { { CAM_PROP_NAME_PASSWORD,	PROP_STRING,	PROP_TO_DEVICE },
            cam_node_prop_init_password_setpoint, cam_node_prop_update_password },
    { { CAM_PROP_NAME_MODEL,	PROP_STRING,	PROP_TO_DEVICE },
            cam_node_prop_init_model_setpoint, cam_node_prop_update_model },
    { { CAM_PROP_NAME_RES_WIDTH,	PROP_INTEGER,	PROP_TO_DEVICE },
            cam_node_prop_init_res_width_setpoint, cam_node_prop_update_res_width },
	{ { CAM_PROP_NAME_RES_HEIGHT,	PROP_INTEGER,	PROP_TO_DEVICE },
			cam_node_prop_init_res_height_setpoint, cam_node_prop_update_res_height },
    { { CAM_PROP_NAME_MAXBITRATE,	PROP_INTEGER,	PROP_TO_DEVICE },
            cam_node_prop_init_bitratemax_setpoint, cam_node_prop_update_bitratemax },
	{ { CAM_PROP_NAME_FLIP,	PROP_INTEGER,	PROP_TO_DEVICE },
			cam_node_prop_init_flip_setpoint, cam_node_prop_update_flip },
};

/*****************************************
 * Node template name to definition mapping
 *****************************************/

struct cam_node_template_entry {
	const char *key;
	const struct cam_node_prop_def * const prop_defs;
	size_t num_props;
};

static const struct cam_node_template_entry cam_node_templates[] = {
	{
		CAM_NODE_TEMPLATE_BASE,
		cam_template_base, ARRAY_LEN(cam_template_base)
	},
	{
        CAM_NODE_TEMPLATE_NODE,
		cam_template_node, ARRAY_LEN(cam_template_node)
	},
};

/*****************************************
 * Node and property setup
 *****************************************/

struct cam_node_search_query {
	bool (*match)(const struct cam_node_state *cam_node, const void *);
	const void *arg;
	struct cam_node_state *result;
};

/*
 * Helper function to get the node state from the node's
 * network state slot.
 */
static inline struct cam_node_state *cam_node_state_get(struct node *node)
{
	return (struct cam_node_state *)node_state_get(node, STATE_SLOT_NET);
}

/*
 * Query function intended to be used with node_foreach() to search
 * for a node using the supplied cam_node_search_query.
 */
static int cam_node_search_handler(struct node *node, void *arg)
{
	struct cam_node_search_query *query =
	    (struct cam_node_search_query *)arg;
	struct cam_node_state *cam_node = cam_node_state_get(node);

	ASSERT(query != NULL);
	ASSERT(query->match != NULL);

	if (!cam_node) {
		return 0;
	}
	if (!query->match(cam_node, query->arg)) {
		/* Not a match; keep searching */
		return 0;
	}
	/* Found match, so return > 0 to break foreach loop */
	query->result = cam_node;
	return 1;
}

/*
 * Find the template definition for a particular property.
 * This is useful to restore the state for nodes loaded from config.
 */
static const struct cam_node_prop_def *cam_node_template_lookup(
	const struct node_prop *prop)
{
	const struct cam_node_template_entry *template = cam_node_templates;
	size_t num_templates = ARRAY_LEN(cam_node_templates);
	const struct cam_node_prop_def *prop_def;
	size_t num_props;

	/* Lookup the template by name */
	for (; num_templates; --num_templates, ++template) {
		if (!strcmp(template->key, prop->template->key)) {
			break;
		}
	}
	if (!num_templates) {
		return NULL;
	}
	/* Lookup the property by name */
	prop_def = template->prop_defs;
	num_props = template->num_props;
	for (; num_props; --num_props, ++prop_def) {
		if (!strcmp(prop_def->def.name, prop->name)) {
			return prop_def;
		}
	}
	return NULL;
}

/*
 * Initial setup of a node property.
 */
static int cam_node_prop_init(struct node *node, struct node_prop *prop,
	void *arg)
{
	const struct cam_node_prop_def *prop_def =
	    (const struct cam_node_prop_def *)arg;

	if (!prop_def) {
		/* Property definition not supplied, so try to look it up */
		prop_def = cam_node_template_lookup(prop);
		if (!prop_def) {
			log_warn("%s: property %s is not managed",
			    node->addr, prop->name);
			return 0;
		}
	}
	/* Assign definition as state for access to callbacks */
	node_prop_state_set(prop, STATE_SLOT_NET, (void *)prop_def, NULL);
	/* Initialize property state */
	if (prop_def->init) {
		prop_def->init(node, prop);
	}
	return 0;
}

/*
 * Associate a node template definition table with a node.  Used by the
 * to setup a node supporting the desired characteristics.
 */
static void cam_node_template_add(struct node *node,
	const char *subdevice, const char *template,
	const struct cam_node_prop_def *table, size_t table_size)
{
	struct node_prop *prop;

	for (; table_size; --table_size, ++table) {
		prop = node_prop_add(node, subdevice, template, &table->def,
		    NULL);
		if (prop) {
			cam_node_prop_init(node, prop, (void *)table);
		}
	}
}

/*
 * Initialize the "kvs_enable" property.
 */
static void cam_node_prop_init_kvs_enable(struct node *node,
										  struct node_prop *prop)
{
	/* Enabled by default */
	node_prop_boolean_send(node, prop, CAM_KVS_ENABLE_DEFAULT);
}

/*
 * Initialize the "webrtc_enable" property.
 */
static void cam_node_prop_init_webrtc_enable(struct node *node,
										  struct node_prop *prop)
{
	/* Enabled by default */
	node_prop_boolean_send(node, prop, CAM_WEBRTC_ENABLE_DEFAULT);
}

static void kvs_start_delayed_streaming(struct cam_node_state *cam_node, int delay_ms)
{
	timer_set(app_get_timers(), &cam_node->hls_stream_state.start_delay_timer, delay_ms);	/* Start the KVS Streaming after 1 second to delay other properties being setup. */
}

static void webrtc_start_delayed_streaming(struct cam_node_state *cam_node, int delay_ms)
{
	timer_set(app_get_timers(), &cam_node->webrtc_stream_state.start_delay_timer, delay_ms);	/* Start the WebRTC Streaming after 1 second to delay other properties being setup. */
}

static void stop_kvs_streaming(struct node *node, struct cam_node_state *cam_node)
{
	log_debug("Stopping KVS Streaming");

	timer_cancel(app_get_timers(), &cam_node->hls_stream_state.stream_timer);
	timer_cancel(app_get_timers(), &cam_node->hls_stream_state.stream_update_timer);

	if(cam_node->master_stream_state.fd == 0)
	{
		log_debug("Master stream process not started. No need to stop KVS Streaming");
	}
	else
	{
		int ret = master_stream_send_cmd(SC_TM_CMD_STOP_HLS, cam_node);
		if (ret != 0) {
			log_err("Failed to send stop HLS command to master stream process");
		}
		usleep(3000000);
	}

	kill_kvs_streaming(node);
	usleep(500000);
	kill_master_stream(node);
}

static void stop_webrtc_streaming(struct node *node, struct cam_node_state *cam_node)
{
	log_debug("Stopping WebRTC Streaming");

	timer_cancel(app_get_timers(), &cam_node->webrtc_stream_state.stream_timer);
	timer_cancel(app_get_timers(), &cam_node->webrtc_stream_state.stream_update_timer);

	if(cam_node->master_stream_state.fd == 0)
	{
		log_debug("Master stream process not started. No need to stop WebRTC Streaming");
		return;
	}
	else
	{
		int ret = master_stream_send_cmd(SC_TM_CMD_STOP_WEBRTC, cam_node);
		if (ret != 0) {
			log_err("Failed to send stop WebRTC command to master stream process");
		}
		usleep(3000000);
	}

	kill_webrtc_streaming(node);
	usleep(500000);
	kill_master_stream(node);
}

enum MasterStreamState
{
	MS_STARTED_OR_PARENT,
	MS_CHILD_PROC,
	MS_ERROR
};

static void* master_stream_comm_thread(void* arg)
{
	struct cam_node_state* cam_node = (struct cam_node_state*)arg;
	struct master_stream_state* mss = &cam_node->master_stream_state; 

	ssize_t numRead;
	const int comm_buff_size = 1024;
	char buffer[comm_buff_size];

	// Create socket
	mss->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (mss->fd == -1) {
		perror("Server: socket error");
		return NULL;
	}

	memset(&mss->addr, 0, sizeof(struct sockaddr_un));
	mss->addr.sun_family = AF_UNIX;
	strncpy(mss->addr.sun_path, mss->master_path, sizeof(mss->addr.sun_path) - 1);

	if (connect(mss->fd, (struct sockaddr *)&mss->addr, sizeof(struct sockaddr_un)) == -1) {
		perror("Client: connect error");
		return NULL;
	}

	int cmd;
	mss->running = true;

	while(1)
	{
		// Read from the client
		log_debug("Server: waiting for data...");
		numRead = read(mss->fd, buffer, comm_buff_size);
		if(false == mss->running)
		{
			log_debug("Server: master stream process stopped");
			break;
		}
		if(numRead == 0)
		{
			log_debug("Server: client disconnected");
			close(mss->fd);
			continue;
		}
		else if(numRead > 0)
		{
			buffer[numRead] = '\0';
			log_debug("Server received %zd bytes\n", numRead);
			cmd = buffer[0];
			if(cmd >= SC_TS_CMD_CNT)
			{
				cmd -= 48;  // Convert from ASCII to int
			}

			switch(cmd)
			{
				case SC_TS_CMD_HLS_STARTED:
				{
					log_debug("Received HLS started command");
					break;
				}
				case SC_TS_CMD_HLS_STOPPED:
				{
					log_debug("Received HLS stopped command");
					break;
				}
				case SC_TS_CMD_HLS_ERROR:
				{
					log_debug("Received HLS error command");
					break;
				}
				case SC_TS_CMD_WEBRTC_STARTED:
				{
					log_debug("Received WebRTC started command");
					break;
				}
				case SC_TS_CMD_WEBRTC_STOPPED:
				{
					log_debug("Received WebRTC stopped command");
					break;
				}
				case SC_TS_CMD_WEBRTC_ERROR:
				{
					log_debug("Received WebRTC error command");
					break;
				}
				default:
				{
					log_warn("Received unknown command: %d", cmd);
					break;
				}
			}
		}
		else
		{
			log_debug("Server: read error");
			perror("Server: read error");
			continue;
		}
	}

	return NULL;
}

static int master_stream_send_cmd(enum stream_comm_to_master_cmds cmd, struct cam_node_state* cam_node)
{
	if(cmd == SC_TM_CMD_NULL || cmd >= SC_TM_CMD_CNT)
	{
		log_err("Invalid command");
		return -1;
	}

	ssize_t cnt = send(cam_node->master_stream_state.fd, &cmd, 1, MSG_NOSIGNAL);

	return cnt > 0 ? 0 : -1;
}

static int check_start_master_stream(struct cam_node_state* cam_node)
{
	log_debug("Check master stream process");

	if(cam_node->master_stream_state.pid == 0)
	{
		get_master_stream_sock_path(cam_node->master_stream_state.master_path, sizeof(cam_node->master_stream_state.master_path), cam_node->node);
		cam_node->master_stream_state.hls_port = get_hls_port(cam_node->node);
		cam_node->master_stream_state.webrtc_port = get_webrtc_port(cam_node->node);
		remove(cam_node->master_stream_state.master_path);

		cam_node->master_stream_state.pid = fork();
		if (cam_node->master_stream_state.pid == -1)
		{
			log_err("Master stream process failed to start");
			perror("Fork error");
			return MS_ERROR;
		} else if (cam_node->master_stream_state.pid == 0)
		{ 	// Child process
			log_debug("Master stream process starting at PID: %d", cam_node->master_stream_state.pid);
			start_master_stream(cam_node->node);
			return MS_CHILD_PROC;
		}
		else
		{
			// Parent
			// Wait for communication socket get created
			int timeout = 100;
			struct stat buffer;
			while(timeout-- && stat(cam_node->master_stream_state.master_path, &buffer) != 0)
			{
				log_debug("Wait for master stream socket...");
				usleep(100000);
			}
			if(timeout == 0)
			{
				log_err("Master stream process failed to start");
				return -1;
			}

			int ret = pthread_create(&cam_node->master_stream_state.comm_thread, NULL, master_stream_comm_thread, cam_node);
			if(ret != 0)
			{
				log_err("Failed to create master stream communication thread");
				return -1;
			}

			return MS_STARTED_OR_PARENT;
		}
	}

	log_debug("Master stream process already started at PID: %d", cam_node->master_stream_state.pid);

	return MS_STARTED_OR_PARENT;
}

/*
 * Update the "kvs_enable" property.
 */
static int cam_node_prop_update_kvs_enable(struct node *node,
										  struct node_prop *prop)
{
	ASSERT(prop->type == PROP_BOOLEAN);
	bool enabled = *((bool*)prop->val);

	struct cam_node_state *cam_node = cam_node_state_get(node);
	if(enabled)
	{
		log_debug("Starting KVS Streaming");
		int ret = check_start_master_stream(cam_node);
		if(MS_CHILD_PROC == ret)
		{
			return 0;
		}
		else if(MS_ERROR == ret)
		{
			return -1;
		}

		kvs_start_delayed_streaming(cam_node, STREAM_START_DELAY_MS);
	}
	else
	{
		stop_kvs_streaming(node, cam_node);
	}

	return 0;
}

/*
 * Update the "webrtc_enable" property.
 */
static int cam_node_prop_update_webrtc_enable(struct node *node,
										   struct node_prop *prop)
{
	ASSERT(prop->type == PROP_BOOLEAN);
	bool enabled = *((bool*)prop->val);

	struct cam_node_state *cam_node = cam_node_state_get(node);
	if(enabled)
	{
		log_debug("Starting WebRTC streaming");
		int ret = check_start_master_stream(cam_node);
		if(MS_CHILD_PROC == ret)
		{
			return 0;
		}
		else if(MS_ERROR == ret)
		{
			return -1;
		}

		webrtc_start_delayed_streaming(cam_node, STREAM_START_DELAY_MS);
	}
	else
	{
		stop_webrtc_streaming(node, cam_node);
	}

	return 0;
}

/*
 * Initialize the URL property.
 */
static void cam_node_prop_init_url_setpoint(struct node *node,
                                             struct node_prop *prop)
{
    /* Set default setpoint state (but do not echo it to the cloud) */
    ASSERT(prop->type == PROP_STRING);
    strcpy(prop->val, CAM_URL_DEFAULT);
}

/*
 * Callback to handle a new URL of the camera node.
 */
static int cam_node_prop_update_url(struct node *node,
                                          struct node_prop *prop)
{
	/* Check if the URL contains username and password */
	if(check_url_userpass(prop->val) >= 0)
	{
		// Username and/or password are present. This is not allowed as those credentials are added as separate properties.
		log_err("URL contains username and/or password. This is not allowed as those credentials are added as separate properties.");
		return -1;
	}

    return 0;
}

/*
 * Initialize the user id property.
 */
static void cam_node_prop_init_userid_setpoint(struct node *node,
                                               struct node_prop *prop)
{
    ASSERT(prop->type == PROP_STRING);
    strcpy(prop->val, CAM_USERID_DEFAULT);
}

/*
 * Update the user id property.
 */
static int cam_node_prop_update_userid(struct node *node,
                                       struct node_prop *prop)
{
	return 0;
}

/*
 * Initialize the password property.
 */
static void cam_node_prop_init_password_setpoint(struct node *node,
                                                 struct node_prop *prop)
{
    ASSERT(prop->type == PROP_STRING);
    strcpy(prop->val, CAM_PASSWORD_DEFAULT);
}

/*
 * Update the password property.
 */
static int cam_node_prop_update_password(struct node *node,
                                         struct node_prop *prop)
{
	return 0;
}

/*
 * Initialize the model property.
 */
static void cam_node_prop_init_model_setpoint(struct node *node,
                                              struct node_prop *prop)
{
    ASSERT(prop->type == PROP_STRING);
    strcpy(prop->val, CAM_MODEL_DEFAULT);
}

/*
 * Update the model property.
 */
static int cam_node_prop_update_model(struct node *node,
                                      struct node_prop *prop)
{
    return 0;
}

/*
 * Initialize the resolution property.
 */
static void cam_node_prop_init_res_width_setpoint(struct node *node,
                                                struct node_prop *prop)
{
    ASSERT(prop->type == PROP_INTEGER);
	*((int*)prop->val) = CAM_RES_WIDTH_DEFAULT;
}

/*
 * Update the resolution property.
 */
static int cam_node_prop_update_res_width(struct node *node,
                                        struct node_prop *prop)
{
    return 0;
}

/*
 * Initialize the resolution property.
 */
static void cam_node_prop_init_res_height_setpoint(struct node *node,
												  struct node_prop *prop)
{
	ASSERT(prop->type == PROP_INTEGER);
	*((int*)prop->val) = CAM_RES_HEIGHT_DEFAULT;
}

/*
 * Update the resolution property.
 */
static int cam_node_prop_update_res_height(struct node *node,
										  struct node_prop *prop)
{
	return 0;
}

/*
 * Initialize the max bitrate property.
 */
static void cam_node_prop_init_bitratemax_setpoint(struct node *node,
                                             struct node_prop *prop)
{
    ASSERT(prop->type == PROP_INTEGER);
    *((int*)prop->val) = CAM_BITRATE_MAX_DEFAULT;
}

/*
 * Update the max bitrate property.
 */
static int cam_node_prop_update_bitratemax(struct node *node,
                                     struct node_prop *prop)
{
    return 0;
}

/*
 * Initialize the max stream_time property.
 */
static void cam_node_prop_init_stream_time_setpoint(struct node *node,
												   struct node_prop *prop)
{
	ASSERT(prop->type == PROP_INTEGER);
	*((int*)prop->val) = CAM_STREAM_TIME_DEFAULT;
}

/*
 * Update the max stream_time property.
 */
static int cam_node_prop_update_stream_time(struct node *node,
										   struct node_prop *prop)
{
	int* new_val = (int*)prop->val;
	struct cam_node_state *cam_node = cam_node_state_get(node);
	if(0 < *new_val) {
		if (timer_active(&cam_node->hls_stream_state.stream_timer)) {
			timer_set(app_get_timers(), &cam_node->hls_stream_state.stream_timer, *new_val);
		}
		if (timer_active(&cam_node->webrtc_stream_state.stream_timer)) {
			timer_set(app_get_timers(), &cam_node->webrtc_stream_state.stream_timer, *new_val);
		}
	}

	return 0;
}

/*
 * Initialize the max storage_size property.
 */
static void cam_node_prop_init_storage_size_setpoint(struct node *node,
													struct node_prop *prop)
{
	ASSERT(prop->type == PROP_INTEGER);
	*((int*)prop->val) = CAM_STORAGE_SIZE_DEFAULT;
}

/*
 * Update the max storage_size property.
 */
static int cam_node_prop_update_storage_size(struct node *node,
											struct node_prop *prop)
{
	return 0;
}

/*
 * Force a kvs stream update.
 */
static int cam_node_request_kvs_stream_update(struct node *node)
{
	struct node_prop * stream_update_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_KVS_STREAM_UPDATE);

	CHK_RET(node_prop_integer_send(node, stream_update_prop, 1));
	CHK_RET(node_prop_integer_send(node, stream_update_prop, 0));

	return 0;
}

/*
 * Force a webrtc stream update.
 */
static int cam_node_request_webrtc_stream_update(struct node *node)
{
	struct node_prop * stream_update_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_WEBRTC_STREAM_UPDATE);

	CHK_RET(node_prop_integer_send(node, stream_update_prop, 1));
	CHK_RET(node_prop_integer_send(node, stream_update_prop, 0));

	return 0;
}

/*
 * Initialize the kvs stream_update property.
 */
static void cam_node_prop_init_kvs_stream_update_setpoint(struct node *node,
													 struct node_prop *prop)
{
	ASSERT(prop->type == PROP_INTEGER);
	*((int*)prop->val) = CAM_KVS_STREAM_UPDATE_DEFAULT;
}

/*
 * Update the kvs stream_update property.
 */
static int cam_node_prop_update_kvs_stream_update(struct node *node,
											 struct node_prop *prop)
{
	int val = *((int*)prop->val);
	if(val == 1)
	{
		CHK_RET(node_prop_integer_send(node, prop, 0));
		CHK_RET(node_prop_integer_send(node, prop, 1));
		CHK_RET(node_prop_integer_send(node, prop, 0));
	}

	return 0;
}

/*
 * Initialize the webrtc stream_update property.
 */
static void cam_node_prop_init_webrtc_stream_update_setpoint(struct node *node,
														  struct node_prop *prop)
{
	ASSERT(prop->type == PROP_INTEGER);
	*((int*)prop->val) = CAM_WEBRTC_STREAM_UPDATE_DEFAULT;
}

/*
 * Update the webrtc stream_update property.
 */
static int cam_node_prop_update_webrtc_stream_update(struct node *node,
												  struct node_prop *prop)
{
	int val = *((int*)prop->val);
	if(val == 1)
	{
		CHK_RET(node_prop_integer_send(node, prop, 0));
		CHK_RET(node_prop_integer_send(node, prop, 1));
		CHK_RET(node_prop_integer_send(node, prop, 0));
	}

	return 0;
}

/*
 * Initialize the max flip property.
 */
static void cam_node_prop_init_flip_setpoint(struct node *node,
													 struct node_prop *prop)
{
	ASSERT(prop->type == PROP_INTEGER);
	*((int*)prop->val) = CAM_FLIP_DEFAULT;
}

/*
 * Update the max flip property.
 */
static int cam_node_prop_update_flip(struct node *node,
											 struct node_prop *prop)
{
	return 0;
}

/*
 * Add properties to a new node.
 */
static void cam_node_populate_props(struct node *node, enum camera_node_type type)
{
	/* All nodes support a cam template */
	cam_node_template_add(node, CAM_NODE_SUBDEVICE, CAM_NODE_TEMPLATE_BASE,
                          cam_template_base, ARRAY_LEN(cam_template_base));

	/* Apply node-specific templates */
	switch (type) {
	case CAMERA:
		cam_node_template_add(node, CAM_NODE_SUBDEVICE,
							  CAM_NODE_TEMPLATE_NODE,
							  cam_template_node, ARRAY_LEN(cam_template_node));
		break;
	default:
		log_err("unsupported cam_node_type");
		ASSERT_NOTREACHED();
	}
}

/*
 * Cleanup function for cam_node_state.
 */
static void cam_node_state_cleanup(void *arg)
{
	struct cam_node_state *node_state = (struct cam_node_state *)arg;

	timer_cancel(state.timers, &node_state->sample_timer);
	free(node_state);
}

static void stream_state_init(struct stream_state* ss, struct node* node,
		void (*stream_timeout)(struct timer *timer), int timeout,
		void (*start_delay_timeout)(struct timer *timer),
	    void (*stream_update_timeout)(struct timer *timer),
		void* stream_data)
{
	memset(ss, 0, sizeof(*ss));
	ss->pid = -1;

	if(NULL != stream_timeout) {
		ss->stream_timer.data = node;
		timer_init(&ss->stream_timer, stream_timeout);
	}

	ss->start_delay_timer.data = node;
	timer_init(&ss->start_delay_timer, start_delay_timeout);

	ss->stream_update_timer.data = node;
	timer_init(&ss->stream_update_timer, stream_update_timeout);

	ss->stream_data = stream_data;
}

/*
 * Associate node state with a node and schedule it to run.
 */
static struct cam_node_state *cam_node_start(struct node *node,
	enum camera_node_type type, unsigned sample_ms)
{
	struct cam_node_state *node_state;

	node_state = (struct cam_node_state *)calloc(1,
	    sizeof(struct cam_node_state));
	if (!node_state) {
		log_err("malloc failed");
		return NULL;
	}
	node_state->node = node;
	node_state->type = type;

	/* Associate state with node entity */
	node_state_set(node, STATE_SLOT_NET, node_state,
	    cam_node_state_cleanup);

//	pthread_mutex_init(&node_state->gst_data.mutex, NULL);

	struct node_prop * stream_time_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_STREAM_TIME);
	stream_state_init(&node_state->hls_stream_state, node, kvs_streaming_timeout, *((int*)stream_time_prop->val), kvs_streaming_start_delay_timeout, cam_kvs_stream_update_timeout, &node_state->hls_data);
	stream_state_init(&node_state->webrtc_stream_state, node, NULL, 0, webrtc_streaming_start_delay_timeout, cam_webrtc_stream_update_timeout, &node_state->webrtc_data);

	timer_init(&node_state->sample_timer, cam_node_sample_timeout);
	node_state->sample_ms = sample_ms ? sample_ms :
							CAM_NODE_SAMPLE_TIME_DEFAULT_MS;
	timer_set(app_get_timers(), &node_state->sample_timer,
			  node_state->sample_ms);
	
	return node_state;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to populate the nodes information and properties.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int cam_node_query_info_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	struct cam_node_state *cam_node = cam_node_state_get(node);

	if (!cam_node) {
		log_err("%s: missing node state", node->addr);
		return -1;
	}
	/*
	 * Query the node's capabilities.  Since this is a simulator, we
	 * already know what the node type is and can look up its information
	 * locally.  When using a real network stack, a gateway might send a
	 * message to the node requesting this info.
	 */
	cam_node_populate_props(node, cam_node->type);
	log_app("%s: configured as %s node", node->addr,
	    cam_node_type_names[cam_node->type]);
	/*
	 * Normally, this callback would be invoked later, when a response
	 * came back from the node, but the node management layer supports
	 * calling it immediately.
	 */
	if (callback) {
		callback(node, NETWORK_SUCCESS);
	}
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to perform any setup operations required to manage the
 * node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int cam_node_configure_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	/*
	 * Simulated nodes do not need to be configured, so just update their
	 * online status.  When using a real network, node features such as
	 * push updates for properties might be setup at this time.
	 */
	node_conn_status_changed(node, true);
	if (callback) {
		callback(node, NETWORK_SUCCESS);
	}
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to send a new property value to the node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int cam_node_prop_set_handler(struct node *node, struct node_prop *prop,
    void (*callback)(struct node *, struct node_prop *,
    enum node_network_result))
{
	const struct cam_node_prop_def *prop_def =
	    (const struct cam_node_prop_def *)node_prop_state_get(
	    prop, STATE_SLOT_NET);

	/*
	 * Invoke optional set callback set when the
	 * node was initialized.
	 */
	if (prop_def && prop_def->set_callback) {
		if (prop_def->set_callback(node, prop) < 0) {
			return -1;
		}
	}
	if (node->online) {
		/* Using simulated nodes, so there is nothing to do here */
		log_info("%s: property %s set to %s", node->addr, prop->name,
		    prop_val_to_str(prop->val, prop->type));
		if (callback) {
			callback(node, prop, NETWORK_SUCCESS);
		}
	} else if (callback) {
		callback(node, prop, NETWORK_OFFLINE);
	}
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer to remove the node from the network.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int cam_node_leave_handler(struct node *node,
    void (*callback)(struct node *, enum node_network_result))
{
	log_app("%s: leaving network", node->addr);
	if (callback) {
		callback(node, NETWORK_SUCCESS);
	}
	/*
	 * Using simulated nodes, so there is nothing to do here.
	 * Simulator state will be cleaned up automatically when the
	 * generic node management state machine deletes the node.
	 */
	return 0;
}

/*
 * Handler called by the generic node management layer to prompt the network
 * interface layer push a downloaded OTA firmware image to a node.
 * If callback is supplied and this function returns 0,
 * callback MUST be invoked when the operation completes.
 */
static int cam_node_ota_handler(struct node *node,
	const char *version, const char *image_path,
	void (*callback)(struct node *, enum node_network_result))
{
	if (node->online) {
		/* Assume OTA load success for simulated nodes */
		log_app("%s: upgraded to version %s", node->addr, version);
		node_info_changed(node, version);
		if (callback) {
			callback(node, NETWORK_SUCCESS);
		}
	} else if (callback) {
		callback(node, NETWORK_OFFLINE);
	}
	return 0;
}

#define SAVE_KVS_STREAM_DATA_STR(name, data) \
	if(NULL == data) { \
		CHK_RET(json_object_set_new(net_state_obj, get_kvs_data_str(name), json_string(""))); \
	} else { \
		CHK_RET(json_object_set_new(net_state_obj, get_kvs_data_str(name), json_string(data))); \
	}
static int cam_node_save_kvs_data(json_t* net_state_obj, const struct hls_data* kvs_data)
{
	SAVE_KVS_STREAM_DATA_STR(KVS_CHANNEL_NAME, kvs_data->kvs_channel_name);
	SAVE_KVS_STREAM_DATA_STR(KVS_ARN, kvs_data->arn);
	SAVE_KVS_STREAM_DATA_STR(KVS_REGION, kvs_data->region);
	SAVE_KVS_STREAM_DATA_STR(KVS_ACCESS_KEY_ID, kvs_data->access_key_id);
	SAVE_KVS_STREAM_DATA_STR(KVS_SECRET_ACCESS_KEY, kvs_data->secret_access_key);
	SAVE_KVS_STREAM_DATA_STR(KVS_SESSION_TOKEN, kvs_data->session_token);
	CHK_RET(json_object_set_new(net_state_obj, get_kvs_data_str(KVS_EXPIRATION_TIME), json_integer(kvs_data->expiration_time)));
	CHK_RET(json_object_set_new(net_state_obj, get_kvs_data_str(KVS_RETENTION_DAYS), json_integer(kvs_data->retention_days)));

	return 0;
}

#define SAVE_WEBRTC_STREAM_DATA_STR(name, data) \
	if(NULL == data) { \
		CHK_RET(json_object_set_new(net_state_obj, get_webrtc_data_str(name), json_string(""))); \
	} else { \
		CHK_RET(json_object_set_new(net_state_obj, get_webrtc_data_str(name), json_string(data))); \
	}
static int cam_node_save_webrtc_data(json_t* net_state_obj, const struct webrtc_data* webrtc_data)
{
	SAVE_WEBRTC_STREAM_DATA_STR(WEBRTC_CHANNEL_NAME, webrtc_data->webrtc_channel_name);
	SAVE_WEBRTC_STREAM_DATA_STR(WEBRTC_ARN, webrtc_data->arn);
	SAVE_WEBRTC_STREAM_DATA_STR(WEBRTC_REGION, webrtc_data->region);
	SAVE_WEBRTC_STREAM_DATA_STR(WEBRTC_ACCESS_KEY_ID, webrtc_data->access_key_id);
	SAVE_WEBRTC_STREAM_DATA_STR(WEBRTC_SECRET_ACCESS_KEY, webrtc_data->secret_access_key);
	SAVE_WEBRTC_STREAM_DATA_STR(WEBRTC_SESSION_TOKEN, webrtc_data->session_token);
	CHK_RET(json_object_set_new(net_state_obj, get_webrtc_data_str(WEBRTC_EXPIRATION_TIME), json_integer(webrtc_data->expiration_time)));

	return 0;
}

static int cam_node_load_kvs_data(const json_t* net_state_obj, struct hls_data* kvs_data)
{
	/* Check KVS data structure for initialized data */
	if(NULL != kvs_data->kvs_channel_name ||
		NULL != kvs_data->arn ||
		NULL != kvs_data->region ||
		NULL != kvs_data->access_key_id ||
		NULL != kvs_data->secret_access_key ||
		NULL != kvs_data->session_token)
	{
		return -1;	// KVS data already initialized. Memory leak risk.
	}

	kvs_data->kvs_channel_name = json_get_string_dup(net_state_obj, get_kvs_data_str(KVS_CHANNEL_NAME)); CHK_PTR(kvs_data->kvs_channel_name);
	kvs_data->arn = json_get_string_dup(net_state_obj, get_kvs_data_str(KVS_ARN)); CHK_PTR(kvs_data->arn);
	kvs_data->region = json_get_string_dup(net_state_obj, get_kvs_data_str(KVS_REGION)); CHK_PTR(kvs_data->region);
	kvs_data->access_key_id = json_get_string_dup(net_state_obj, get_kvs_data_str(KVS_ACCESS_KEY_ID)); CHK_PTR(kvs_data->access_key_id);
	kvs_data->secret_access_key = json_get_string_dup(net_state_obj, get_kvs_data_str(KVS_SECRET_ACCESS_KEY)); CHK_PTR(kvs_data->secret_access_key);
	kvs_data->session_token = json_get_string_dup(net_state_obj, get_kvs_data_str(KVS_SESSION_TOKEN)); CHK_PTR(kvs_data->session_token);

	if (json_get_int(net_state_obj, get_kvs_data_str(KVS_EXPIRATION_TIME), &kvs_data->expiration_time) < 0) {
		return -1;
	}
	if (json_get_int(net_state_obj, get_kvs_data_str(KVS_RETENTION_DAYS), &kvs_data->retention_days) < 0) {
		return -1;
	}

	return 0;
}

static int cam_node_load_webrtc_data(const json_t* net_state_obj, struct webrtc_data* webrtc_data)
{
	/* Check WEBRTC data structure for initialized data */
	if(NULL != webrtc_data->webrtc_channel_name ||
	   NULL != webrtc_data->arn ||
	   NULL != webrtc_data->region ||
	   NULL != webrtc_data->access_key_id ||
	   NULL != webrtc_data->secret_access_key ||
	   NULL != webrtc_data->session_token)
	{
		return -1;	// WEBRTC data already initialized. Memory leak risk.
	}

	webrtc_data->webrtc_channel_name = json_get_string_dup(net_state_obj, get_webrtc_data_str(WEBRTC_CHANNEL_NAME)); CHK_PTR(webrtc_data->webrtc_channel_name);
	webrtc_data->arn = json_get_string_dup(net_state_obj, get_webrtc_data_str(WEBRTC_ARN)); CHK_PTR(webrtc_data->arn);
	webrtc_data->region = json_get_string_dup(net_state_obj, get_webrtc_data_str(WEBRTC_REGION)); CHK_PTR(webrtc_data->region);
	webrtc_data->access_key_id = json_get_string_dup(net_state_obj, get_webrtc_data_str(WEBRTC_ACCESS_KEY_ID)); CHK_PTR(webrtc_data->access_key_id);
	webrtc_data->secret_access_key = json_get_string_dup(net_state_obj, get_webrtc_data_str(WEBRTC_SECRET_ACCESS_KEY)); CHK_PTR(webrtc_data->secret_access_key);
	webrtc_data->session_token = json_get_string_dup(net_state_obj, get_webrtc_data_str(WEBRTC_SESSION_TOKEN)); CHK_PTR(webrtc_data->session_token);

	if (json_get_int(net_state_obj, get_webrtc_data_str(WEBRTC_EXPIRATION_TIME), &webrtc_data->expiration_time) < 0) {
		return -1;
	}

	return 0;
}

json_t *cam_node_save(const struct node *node)
{
	const struct cam_node_state *cam_node =
	    cam_node_state_get((struct node *)node);
	json_t *net_state_obj;

	if (!cam_node) {
		log_err("%s: missing node state", node->addr);
		return NULL;
	}
	net_state_obj = json_object();
	json_object_set_new(net_state_obj, "cam_type",
	    json_integer(cam_node->type));
	json_object_set_new(net_state_obj, "sample_ms",
	    json_integer(cam_node->sample_ms));

	/* Save KVS data */
	cam_node_save_kvs_data(net_state_obj, &cam_node->hls_data);
	
	/* Save WebRTC data */
	cam_node_save_webrtc_data(net_state_obj, &cam_node->webrtc_data);
	
	return net_state_obj;
}

/*
 * Handler called by the generic node management layer to restore a node
 * loaded from config.  The JSON object output by cam_node_save() is
 * passed in via the net_state_obj parameter.
 */
static int cam_node_loaded(struct node *node, json_t *net_state_obj)
{
	u32 index;
	char *cp;
	enum camera_node_type type;
	unsigned sample_ms;
	unsigned val;

	/* Update index to assign unique addresses for future nodes */
	cp = strrchr(node->addr, '_');
	if (cp) {
		index = strtoul(++cp, NULL, 16);
		if (state.node_index < index) {
			state.node_index = index;
		}
	}
	if (!net_state_obj) {
		log_err("%s: no state", node->addr);
		return -1;
	}
	/* Restore node state stored in the config file by cam_node_save() */
	if (json_get_uint(net_state_obj, "cam_type", &val) < 0 ||
	    val >= CAMERA_TYPE_COUNT) {
		log_err("invalid cam_type");
		return -1;
	}
	type = val;
	if (json_get_uint(net_state_obj, "sample_ms", &sample_ms) < 0) {
		sample_ms = 0;
	}
	/* Restore node state */
	if (!cam_node_start(node, type, sample_ms)) {
		log_err("%s: failed to initialize", node->addr);
		return -1;
	}

	struct cam_node_state *cam_node =
			cam_node_state_get((struct node *)node);
	
	/* Load KVS data */
	if(cam_node_load_kvs_data(net_state_obj, &cam_node->hls_data) != 0)
	{
		log_err("%s: failed to load kvs data", node->addr);
		return -1;
	}
	cam_kvs_stream_update_exec(cam_node);

	if(cam_node_load_webrtc_data(net_state_obj, &cam_node->webrtc_data) != 0)
	{
		log_err("%s: failed to load webrtc data", node->addr);
		return -1;
	}
	cam_webrtc_stream_update_exec(cam_node);

	/*
	 * Call init routine for each property.  Supply NULL
	 * arg so init function looks up template definition.
	 */
	node_prop_foreach(node, cam_node_prop_init, NULL);
	log_app("%s: loaded from config", node->addr);

	/* This is a node, so set it to online */
	node_conn_status_changed(node, true);

	return 0;
}

///*****************************************
// * Control functions
// *****************************************/
//
/*
 * Match function to search for nodes by type.  Ignores nodes that have been
 * flagged for removal.
 */
static bool cam_node_search_by_type(const struct cam_node_state *cam_node,
	const void *arg)
{
	enum camera_node_type type;

	ASSERT(arg != NULL);
	type = *(enum camera_node_type *)arg;
	return !cam_node->pending_removal && cam_node->type == type;
}

/*
 * Handler for step timer.  Updates global state.
 */
static void cam_step_timeout(struct timer *timer)
{
//	struct camera_state *cam = CONTAINER_OF(struct camera_state, step_timer,
//	    timer);

//	cam_update_simulation(cam);

	timer_set(app_get_timers(), &state.step_timer, CAM_STEP_TIME_MS);
}

static bool check_stream_force_update(struct stream_state* ss, int expire_time)
{
	// get current unix time
	time_t current_time;
	time(&current_time);

	// get delta time
	int delta_time = difftime(expire_time, current_time);
	log_debug("Stream needs to be updated in : %d [sec]", delta_time);

	// check if delta time is greater than threshold
	if(delta_time < CAM_STREAM_TIME_DIFF_THRESHOLD)
	{
		log_debug("Stream is going to be updated");
		return true;
	}

	log_debug("Stream is not going to be updated");
	return false;
}

static void cam_kvs_stream_update_exec(struct cam_node_state *cam_node)
{
	log_debug("KVS stream update timeout");

	if(check_stream_force_update(&cam_node->hls_stream_state, cam_node->hls_data.expiration_time))
	{
		// Stream needs to update credentials
		log_debug("KVS stream update requested");

		stop_kvs_streaming(cam_node->node, cam_node);
		cam_node_request_kvs_stream_update(cam_node->node);
		kvs_start_delayed_streaming(cam_node, STREAM_START_DELAY_MS * 2);
	}
}

/*
 * Handler for KVS stream update timeout.
 */
static void cam_kvs_stream_update_timeout(struct timer *timer)
{
	struct node* node = (struct node*)timer->data;
	struct cam_node_state *cam_node = cam_node_state_get(node);

	cam_kvs_stream_update_exec(cam_node);

	timer_set(app_get_timers(), &cam_node->hls_stream_state.stream_update_timer, CAM_STREAM_UPDATE_TIME_MS);
}

static void cam_webrtc_stream_update_exec(struct cam_node_state *cam_node)
{
	log_debug("WebRTC stream update timeout");

	if(check_stream_force_update(&cam_node->webrtc_stream_state, cam_node->webrtc_data.expiration_time))
	{
		// Stream needs to update credentials
		log_debug("WebRTC stream update requested");

		stop_webrtc_streaming(cam_node->node, cam_node);
		cam_node_request_webrtc_stream_update(cam_node->node);
		webrtc_start_delayed_streaming(cam_node, STREAM_START_DELAY_MS * 2);
	}
}

/*
 * Handler for WebRTC stream update timeout.
 */
static void cam_webrtc_stream_update_timeout(struct timer *timer)
{
	struct node* node = (struct node*)timer->data;
	struct cam_node_state *cam_node = cam_node_state_get(node);

	cam_webrtc_stream_update_exec(cam_node);

	timer_set(app_get_timers(), &cam_node->webrtc_stream_state.stream_update_timer, CAM_STREAM_UPDATE_TIME_MS);
}

static void cam_debug_print_props(struct node *node)
{
#define NODE_CHECK_PTR(ptr) if ((ptr) == NULL) { log_debug("NULL pointer"); return; }

    NODE_CHECK_PTR(node);
    struct node_prop *url_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_URL); NODE_CHECK_PTR(url_prop);
    struct node_prop *user_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_USERID); NODE_CHECK_PTR(user_prop);
    struct node_prop *pass_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_PASSWORD); NODE_CHECK_PTR(pass_prop);
    struct node_prop *model_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_MODEL); NODE_CHECK_PTR(model_prop);
    struct node_prop *res_width_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_RES_WIDTH); NODE_CHECK_PTR(res_width_prop);
    struct node_prop *res_height_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_RES_HEIGHT); NODE_CHECK_PTR(res_height_prop);
    struct node_prop *bitratemax_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_MAXBITRATE); NODE_CHECK_PTR(bitratemax_prop);
    struct node_prop *flip_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_FLIP); NODE_CHECK_PTR(flip_prop);

    log_debug("URL: %s", (const char*)url_prop->val);
    log_debug("User: %s", (const char*)user_prop->val);
    log_debug("Pass: %s", (const char*)pass_prop->val);
    log_debug("Model: %s", (const char*)model_prop->val);
    log_debug("Res. width: %d", *(int*)res_width_prop->val);
    log_debug("Res. height: %d", *(int*)res_height_prop->val);
    log_debug("Bitrate max: %d", *((int*)bitratemax_prop->val));
    log_debug("Flip: %d", *((int*)flip_prop->val));

	struct cam_node_state* node_state = cam_node_state_get(node);
	// Prints kvsdata structure elements from node_state
	log_debug("kvsdata channel_name: %s", node_state->hls_data.kvs_channel_name);
	log_debug("kvsdata arn: %s", node_state->hls_data.arn);
	log_debug("kvsdata aws_access_key: %s", node_state->hls_data.access_key_id);
	log_debug("kvsdata aws_secret_key: %s", node_state->hls_data.secret_access_key);
	log_debug("kvsdata aws_region: %s", node_state->hls_data.region);
	log_debug("kvsdata session_token: %s", node_state->hls_data.session_token);
	log_debug("kvsdata expiration_time: %d", node_state->hls_data.expiration_time);
	log_debug("kvsdata retention_days: %d", node_state->hls_data.retention_days);

	// Prints webrtcdata structure elements from node_state
	log_debug("webrtcdata channel_name: %s", node_state->webrtc_data.webrtc_channel_name);
	log_debug("webrtcdata arn: %s", node_state->webrtc_data.arn);
	log_debug("webrtcdata aws_access_key: %s", node_state->webrtc_data.access_key_id);
	log_debug("webrtcdata aws_secret_key: %s", node_state->webrtc_data.secret_access_key);
	log_debug("webrtcdata aws_region: %s", node_state->webrtc_data.region);
	log_debug("webrtcdata session_token: %s", node_state->webrtc_data.session_token);
	log_debug("webrtcdata expiration_time: %d", node_state->webrtc_data.expiration_time);
}

//static int cam_node_start_video_stream_control(struct gst_data *gst_data, struct cam_node_state* node_state)
//{
//	log_debug("Initializing video stream control");
//
////	// Create a socket pair
////	if (socketpair(AF_UNIX, SOCK_STREAM, 0, gst_data->sockfd) == -1) {
////		log_err("Failed to create socket pair");
////		perror("socketpair");
////		return -1;
////	}
//
//	// Fork a child process
//	gst_data->pid = fork();
//	if (gst_data->pid == -1) {
//		log_err("Failed to fork");
//		perror("fork");
//		return -2;
//	} else if (gst_data->pid == 0)
//	{ // Child process
//
//		start_master_stream(node_state->node);
//		/*
//		close(gst_data->sockfd[1]); // Close the unused end of the socket pair
//
//		if(gst_video_stream_start_child_comm_thread(&gst_data) != 0) {
//			return -3;
//		}
//
//		int ret = gst_video_stream_init(&gst_data);
//		if(0 != ret)
//		{
//			log_err("Failed to initialize Gst");
//			return -4;
//		}
//
//		ret = gst_start_pipeline_thread(&gst_data);
//		if(0 != ret)
//		{
//			log_err("Failed to start gst pipeline thread");
//			return -5;
//		}*/
//
//	} else
//	{ // Parent process
//		/*
//		close(gst_data->sockfd[0]); // Close the unused end of the socket pair
//
//		if(master_stream_comm_thread(&gst_data) != 0) {
//			return -5;
//		}
//
//		pthread_mutex_lock(&gst_data->mutex);
//		gst_data->initialized = true;
//		pthread_mutex_unlock(&gst_data->mutex);*/
//	}
//
//	return 0;
//}

/*
 * Handler for node sample timer.  Updates node state.
 */
static void cam_node_sample_timeout(struct timer *timer)
{
	struct cam_node_state *node_state = CONTAINER_OF(struct cam_node_state,
	    sample_timer, timer);

	// Print node properties
	static uint32_t print_props_counter;
	if(print_props_counter++ % 100 == 0) {
		cam_debug_print_props(node_state->node);
	}

//	// Check for update HLS and WebRTC stream credentials
//	if(/*(! gst_video_stream_is_initialized(&node_state->gst_data)) &&*/ node_state->hls_data.received && node_state->webrtc_data.received)
//	{
//		// The video stream pipeline is not initialized and we have received HLS and WebRTC credentials
//		// Reset the flags
//		node_state->hls_data.received = false;
//		node_state->webrtc_data.received = false;
//
//		// Initialize video stream control
//		cam_node_start_video_stream_control(&node_state->gst_data, node_state);
//	}

	// Restart timer
	timer_set(app_get_timers(), &node_state->sample_timer,
	    node_state->sample_ms);
}

/*
 * Initialize the node.
 */
void cam_init(struct timer_head *timers)
{
	struct node_network_callbacks callbacks = {
		.node_query_info = cam_node_query_info_handler,
		.node_configure = cam_node_configure_handler,
		.node_prop_set = cam_node_prop_set_handler,
		.node_factory_reset = NULL,
		.node_leave = cam_node_leave_handler,
		.node_ota_update = cam_node_ota_handler,
		.node_conf_save = cam_node_save,
		.node_conf_loaded = cam_node_loaded
	};
	srandom(time(NULL));	/* Seed random number generator */
	state.timers = timers;
	timer_init(&state.step_timer, cam_step_timeout);
	node_set_network_callbacks(&callbacks);
}

/*
 * Start the node.
 */
void cam_start(void)
{
	log_app("starting cam gateway");
	timer_set(app_get_timers(), &state.step_timer, 0);
}

/*
 * Stop the node simulator.
 */
void cam_stop(void)
{
	log_app("stopping cam gateway");
	timer_cancel(app_get_timers(), &state.step_timer);
}

/*
 * Node joining the network.  Sample_secs is optional and the
 * default value will be used if it is set to 0.
 * Return 0 on success or -1 on failure.
 */
int cam_node_add(enum camera_node_type type, unsigned sample_secs)
{
	struct node *node;
	char addr[GW_NODE_ADDR_SIZE];
	char oem_model[GW_MAX_OEM_MODEL_SIZE];

	/* Assign a unique address */
	++state.node_index;
	snprintf(addr, sizeof(addr), "%s_cam_%02X", cam_node_type_names[type],
	    state.node_index);
	/* Node OEM model.  Helps mobile app present custom UI for nodes. */
    snprintf(oem_model, sizeof(oem_model), CAM_NODE_OEM_MODEL_PREFIX "%s",
             cam_node_type_names[type]);
	/* Join node */
	node = node_joined(addr, oem_model, GI_ZIGBEE, GP_MAINS, NULL);
	if (!node) {
        log_err("%s: failed to join", addr);
		return -1;
	}
	/* Initialize node node_state */
	if (!cam_node_start(node, type, sample_secs * 1000)) {
		log_err("%s: failed to initialize", node->addr);
		node_left(node);
		return -1;
	}
	log_app("%s: joined", node->addr);
	return 0;
}

/*
 * Node leaving the network.
 * Return 0 on success or -1 on failure.
 */
int cam_node_remove(enum camera_node_type type)
{
	struct cam_node_search_query query = {
		.match = cam_node_search_by_type,
		.arg = &type,
		.result = NULL };

	/* Search for node to remove by type */
	if (node_foreach(cam_node_search_handler, &query) < 0 ||
	    !query.result) {
		log_warn("no %s node to remove", cam_node_type_names[type]);
		return -1;
	}
	log_app("%s: leaving", query.result->node->addr);
	query.result->pending_removal = true;
	node_left(query.result->node);
	return 0;
}

static void fork_and_start_kvs_streaming(struct node *node)
{
	struct cam_node_state *cam_node = cam_node_state_get(node);

	int ret = master_stream_send_cmd(SC_TM_CMD_START_HLS, cam_node);
	if(ret != 0)
	{
		log_err("Failed to send start HLS command to master stream process");
		return;
	}
	usleep(100000);

	pid_t pid;
	ret = -1;
	if (cam_node->hls_stream_state.pid > 0) {
		log_warn("KVS Streaming already started");
		return;
	}

	log_debug("starting KVS Streaming fork");
	pid = fork();
	if (pid < 0) {
		log_err("fork failed");
		return;
	}
	if (pid == 0) {
		start_kvs_streaming(node);
	} else {
		cam_node->hls_stream_state.pid = pid;
		struct node_prop * stream_time_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_STREAM_TIME);
		int stream_time = *((int*)stream_time_prop->val);
		if(0 < stream_time) {
			timer_set(app_get_timers(), &cam_node->hls_stream_state.stream_timer, (stream_time * 1000));
		}
		timer_set(app_get_timers(), &cam_node->hls_stream_state.stream_update_timer, CAM_STREAM_UPDATE_TIME_MS);
	}
}

static void fork_and_start_webrtc_streaming(struct node *node)
{
	struct cam_node_state *cam_node = cam_node_state_get(node);
	int ret = master_stream_send_cmd(SC_TM_CMD_START_WEBRTC, cam_node);
	if(ret != 0)
	{
		log_err("Failed to send start WebRTC command to master stream process");
		return;
	}
	usleep(10000);

	pid_t pid;
	ret = -1;
	if (cam_node->webrtc_stream_state.pid > 0) {
		log_warn("WebRTC Streaming already started");
		return;
	}
	log_debug("starting WebRTC Streaming fork");
	pid = fork();
	if (pid < 0) {
		log_err("fork failed");
		return;
	}
	if (pid == 0) {
		start_webrtc_streaming(node);
	} else {
		cam_node->webrtc_stream_state.pid = pid;
		timer_set(app_get_timers(), &cam_node->webrtc_stream_state.stream_update_timer, CAM_STREAM_UPDATE_TIME_MS);
	}
}

/*
 * Handle kvs streaming timer timeout
 */
static void kvs_streaming_timeout(struct timer *timer)
{
	log_warn("got timeout for streaming, killing the KVS Stream");
	timer_cancel(app_get_timers(), timer);
	kill_kvs_streaming(timer->data);
}

/*
 * Terminate kvs_streaming, if managed by kvs_streaming.
 */
static void kill_kvs_streaming(struct node* node)
{
	struct cam_node_state *cam_node = cam_node_state_get(node);
	if (cam_node->hls_stream_state.pid > 0) {
		log_debug("Killing KVS stream PID = %d", cam_node->hls_stream_state.pid);
		kill_proc(cam_node->hls_stream_state.pid, 1000);
		cam_node->hls_stream_state.pid = 0;
	}
	else
	{
		log_debug("KVS streaming not running. PID = %d", cam_node->hls_stream_state.pid);
	}

	struct node_prop * prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_KVS_ENABLE);
	node_prop_boolean_send(node, prop, false);
}

/*
 * Terminate webrtc_streaming, if managed by webrtc_streaming.
 */
static void kill_webrtc_streaming(struct node* node) {
	struct cam_node_state *cam_node = cam_node_state_get(node);
	if (cam_node->webrtc_stream_state.pid > 0) {
		log_debug("Killing WebRTC stream PID = %d", cam_node->webrtc_stream_state.pid);
		kill_proc(cam_node->webrtc_stream_state.pid, 1000);
		cam_node->webrtc_stream_state.pid = 0;
	} else {
		log_debug("WebRTC streaming not running. PID = %d", cam_node->webrtc_stream_state.pid);
	}

	struct node_prop *prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_WEBRTC_ENABLE);
	node_prop_boolean_send(node, prop, false);
}

static void kill_master_stream(struct node* node)
{
	struct cam_node_state *cam_node = cam_node_state_get(node);
	if(cam_node->hls_stream_state.pid > 0 || cam_node->webrtc_stream_state.pid > 0)
	{
		log_debug("Can not kill master stream. HLS or WebRTC streaming is running.");
		return;
	}
	else if(cam_node->master_stream_state.pid > 0)
	{
		cam_node->master_stream_state.running = false;
		log_debug("Killing master stream PID = %d", cam_node->master_stream_state.pid);
		kill_proc(cam_node->master_stream_state.pid, 1000);
		cam_node->master_stream_state.pid = 0;
	}
	else
	{
		log_debug("Master streaming not running.");
	}
}

/*
 * Handle kvs streaming start delay timer
 */
static void kvs_streaming_start_delay_timeout(struct timer *timer)
{
	log_info("Starting delayed KVS Stream");
	timer_cancel(app_get_timers(), timer);
	fork_and_start_kvs_streaming(timer->data);
}

/*
 * Handle webrtc streaming start delay timer
 */
static void webrtc_streaming_start_delay_timeout(struct timer *timer)
{
	log_info("Starting delayed WebRTC Stream");
	timer_cancel(app_get_timers(), timer);
	fork_and_start_webrtc_streaming(timer->data);
}

static void start_kvs_streaming(struct node* node)
{
	char storage_size[16];
	char *argv[12];
	char *env[12];
	char aws_key_id[80],aws_secret[80],aws_region[40];
	char aws_session_token[2048];
	char urlfull[512];
	char port[16];
	int i = 0;

	struct cam_node_state *cam_node = cam_node_state_get(node);

	struct node_prop * stor_size_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_STORAGE_SIZE);
	int hls_storage_size = *((int*)stor_size_prop->val);

	struct node_prop * url_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_URL);
	struct node_prop * user_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_USERID);
	struct node_prop * passwd_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_PASSWORD);

	if(get_url_userpass(url_prop->val, user_prop->val, passwd_prop->val, urlfull) < 0)
	{
		log_err("failed to get url with user and password");
		return;
	}

	struct hls_data* kvs_ds = &cam_node->hls_data;

	if( hls_storage_size == 0 )
		//if(key_id == NULL || secret == NULL || region == NULL || (strlen(hls_stream_name) == 0) || hls_storage_size == 0 )
	{
		log_err("did not set the stroage size so not starting HLS-Streaming");
		return;
	}

	snprintf(storage_size,sizeof(storage_size),"%d",hls_storage_size);

	snprintf(aws_key_id,sizeof(aws_key_id),"AWS_ACCESS_KEY_ID=%s",kvs_ds->access_key_id);
	snprintf(aws_secret,sizeof(aws_secret),"AWS_SECRET_ACCESS_KEY=%s",kvs_ds->secret_access_key);
	snprintf(aws_region,sizeof(aws_region),"AWS_DEFAULT_REGION=%s",kvs_ds->region);
	snprintf(aws_session_token,sizeof(aws_session_token),"AWS_SESSION_TOKEN=%s",kvs_ds->session_token);
	snprintf(port, sizeof(port), "%u", cam_node->master_stream_state.hls_port);

//	argv[i++] = SHELL_DEFAULT;
//	argv[i++] = "-c";
	argv[i++] = HLS_STREAM_APP;
//	argv[i++] = urlfull;
	argv[i++] = kvs_ds->kvs_channel_name;
	argv[i++] = storage_size;
	argv[i++] = port;
	argv[i] = NULL;

	ASSERT(i <= ARRAY_LEN(argv));
	//if (debug)
	{
		int j = 0;
		log_debug("Starting %s using args: ", SHELL_DEFAULT);
		for (j = 0; j < i; j++) {
			log_debug("%s", argv[j]);
		}
	}

	log_warn("now setting the env list");
	i = 0;
	env[i++]=GST_PLUGIN_PATH_ENV;
	env[i++]=ADDITIONAL_LIB_PATH_ENV;
	env[i++]=aws_key_id;
	env[i++]=aws_secret;
	env[i++]=aws_region;
	env[i++]=aws_session_token;
	env[i]=NULL;
	{
		int j = 0;
		log_debug("Starting %s using env : ", SHELL_DEFAULT);
		for (j = 0; j < i; j++) {
			log_debug("%s", env[j]);
		}
	}
	log_debug("now executing the KVS Scripts");

//        if( execve(SHELL_DEFAULT, argv, env) == -1)
	if(execve(HLS_STREAM_APP, argv, env) == -1)
		perror("Could not execve");

	/* perhaps running locally on VM */
	log_warn("executing %s failed, trying %s", HLS_STREAM_APP, SHELL_DEFAULT);
	execve(SHELL_DEFAULT, argv, env);
	log_err("unable to start %s", HLS_STREAM_APP);
	sleep(2);
	exit(1);
}

static void start_webrtc_streaming(struct node* node)
{
	char *argv[12];
	char *env[12];
	char aws_key_id[80],aws_secret[80],aws_region[40];
	char aws_session_token[2048];
	char urlfull[512];
	char port[16];
	int i = 0;

	struct cam_node_state *cam_node = cam_node_state_get(node);

	struct node_prop * url_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_URL);
	struct node_prop * user_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_USERID);
	struct node_prop * passwd_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_PASSWORD);

	if(get_url_userpass(url_prop->val, user_prop->val, passwd_prop->val, urlfull) < 0)
	{
		log_err("failed to get url with user and password");
		return;
	}

	struct webrtc_data* kvs_ds = &cam_node->webrtc_data;

	snprintf(aws_key_id,sizeof(aws_key_id),"AWS_ACCESS_KEY_ID=%s",kvs_ds->access_key_id);
	snprintf(aws_secret,sizeof(aws_secret),"AWS_SECRET_ACCESS_KEY=%s",kvs_ds->secret_access_key);
	snprintf(aws_region,sizeof(aws_region),"AWS_DEFAULT_REGION=%s",kvs_ds->region);
	snprintf(aws_session_token,sizeof(aws_session_token),"AWS_SESSION_TOKEN=%s",kvs_ds->session_token);

	snprintf(port, sizeof(port), "%u", cam_node->master_stream_state.webrtc_port);

//	argv[i++] = SHELL_DEFAULT;
//	argv[i++] = "-c";
	argv[i++] = WEBRTC_STREAM_APP;
	argv[i++] = kvs_ds->webrtc_channel_name;
	argv[i++] = port;
	argv[i] = NULL;

	ASSERT(i <= ARRAY_LEN(argv));
	//if (debug)
	{
		int j = 0;
		log_debug("Starting %s using args: ", SHELL_DEFAULT);
		for (j = 0; j < i; j++) {
			log_debug("%s", argv[j]);
		}
	}

	log_warn("now setting the env list");
	i = 0;
	env[i++]=aws_key_id;
	env[i++]=aws_secret;
	env[i++]=aws_region;
	env[i++]=aws_session_token;
	env[i]=NULL;
	{
		int j = 0;
		log_debug("Starting %s using env : ", SHELL_DEFAULT);
		for (j = 0; j < i; j++) {
			log_debug("%s", env[j]);
		}
	}
	log_debug("now executing the WebRTC Scripts");

//        if( execve(SHELL_DEFAULT, argv, env) == -1)
	if(execve(WEBRTC_STREAM_APP, argv , env) == -1)
		perror("Could not execve");

	/* perhaps running locally on VM */
	log_warn("executing %s failed, trying %s", WEBRTC_STREAM_APP, SHELL_DEFAULT);
	execve(SHELL_DEFAULT, argv, env);
	log_err("unable to start %s", WEBRTC_STREAM_APP);
	sleep(2);
	exit(1);
}


static void start_master_stream(struct node* node)
{
	char *argv[12];
	char *env[12];
	char urlfull[512];
	char hls_port[16];
	char webrtc_port[16];
	char width[8];
	char height[8];
	char bitrate[8];
	char flip[8];
	int i = 0;

	struct cam_node_state *cam_node = cam_node_state_get(node);

	struct node_prop * url_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_URL);
	struct node_prop * user_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_USERID);
	struct node_prop * passwd_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_PASSWORD);

	if(get_url_userpass(url_prop->val, user_prop->val, passwd_prop->val, urlfull) < 0)
	{
		log_err("failed to get url with user and password");
		return;
	}

	snprintf(hls_port, sizeof(hls_port), "%u", cam_node->master_stream_state.hls_port);
	snprintf(webrtc_port, sizeof(webrtc_port), "%u", cam_node->master_stream_state.webrtc_port);

	struct node_prop * width_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_RES_WIDTH);
	struct node_prop * height_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_RES_HEIGHT);
	struct node_prop * bitrate_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_MAXBITRATE);
	struct node_prop * flip_prop = node_prop_lookup(node, NULL, NULL, CAM_PROP_NAME_FLIP);
	snprintf(width, sizeof(width), "%d", *((int*)width_prop->val));
	snprintf(height, sizeof(width), "%d", *((int*)height_prop->val));
	snprintf(bitrate, sizeof(width), "%d", *((int*)bitrate_prop->val));
	snprintf(flip, sizeof(width), "%d", *((int*)flip_prop->val));

	argv[i++] = MASTER_STREAM_APP;
	argv[i++] = urlfull;
	argv[i++] = cam_node->master_stream_state.master_path;
	argv[i++] = hls_port;
	argv[i++] = webrtc_port;
	argv[i++] = bitrate;
	argv[i++] = width;
	argv[i++] = height;
	argv[i++] = flip;
	argv[i] = NULL;

	ASSERT(i <= ARRAY_LEN(argv));
	//if (debug)
	{
		int j = 0;
		log_debug("Starting %s using args: ", SHELL_DEFAULT);
		for (j = 0; j < i; j++) {
			log_debug("%s", argv[j]);
		}
	}

	log_warn("now setting the env list");
	i = 0;
	env[i++]="GST_DEBUG=\"*:5\"";
	env[i]=NULL;
	{
		int j = 0;
		log_debug("Starting %s using env : ", SHELL_DEFAULT);
		for (j = 0; j < i; j++) {
			log_debug("%s", env[j]);
		}
	}

	log_debug("now executing the KVS Scripts");

	if(execve(MASTER_STREAM_APP, argv, env) == -1)
		perror("Could not execve");

	/* perhaps running locally on VM */
	log_warn("executing %s failed, trying %s", MASTER_STREAM_APP, SHELL_DEFAULT);
	execve(SHELL_DEFAULT, argv, env);
	log_err("unable to start %s", MASTER_STREAM_APP);
	sleep(2);
	exit(1);
}

static void get_master_stream_sock_path(char* path, size_t path_size, struct node* node)
{
	snprintf(path, path_size, "/tmp/master_stream_socket_%s", node->addr);
}

static u16 get_addr_val(struct node* node)
{
	unsigned int addr = 0;
	if (sscanf(node->addr, "%*[^_]_%*[^_]_%u", &addr) != 1)
	{
		log_err("Failed to parse the string.\n");
	}

	return (u16)addr;
}

static u16 get_hls_port(struct node* node)
{
	u16 addr = get_addr_val(node);
	return STREAM_BASE_PORT + (addr * 2) - 1;
}

static u16 get_webrtc_port(struct node* node)
{
	u16 addr = get_addr_val(node);
	return STREAM_BASE_PORT + (addr * 2);
}

