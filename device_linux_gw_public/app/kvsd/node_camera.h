/*
 * Copyright 2023 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __NODE_CAMERA_H__
#define __NODE_CAMERA_H__

#include "video_stream.h"
#include "ayla/timer.h"
#include <sys/un.h>


/*
 * Types of virtual nodes supported by the simulator.
 */
#define CAMERA_NODE_TYPES(def)				\
	def(camera,		CAMERA)		\
							\
	def(,			CAMERA_TYPE_COUNT)	/* MUST be last entry */

DEF_ENUM(camera_node_type, CAMERA_NODE_TYPES);

/*
 * Streaming state
 */
struct stream_state {
	pid_t pid;						/* Streaming process ID */
	struct timer stream_timer;		/* Timer for streaming */
	struct timer start_delay_timer;	/* Timer for streaming start delay */
	void* stream_data;				/* Streaming data */
	struct timer stream_update_timer;	/* Timer for stream updates */
};

struct master_stream_state {
	pid_t pid;						/* Streaming process ID */
	pthread_t comm_thread;			/* Thread for communication with master stream process */
	bool running;					/* Flag indicating if master stream is running */

	int fd;						/* Socket file descriptor */
	struct sockaddr_un addr;
	char master_path[256];
	u16 hls_port;
	u16 webrtc_port;
};

/*
 * Camera node state
 */
struct cam_node_state {
	struct node *node;		/* Pointer to node */
	enum camera_node_type type;	/* Camera node type */
	unsigned sample_ms;		/* Property update period */
	struct timer sample_timer;	/* Timer for prop updates */
	bool pending_removal;		/* Track node leave event */

	struct master_stream_state master_stream_state;	/* Master stream state */

	struct hls_data hls_data;	/* HLS data for node */
	struct stream_state hls_stream_state;	/* HLS streaming state */

	struct webrtc_data webrtc_data;	/* WebRTC data for node */
	struct stream_state webrtc_stream_state;	/* WebRTC streaming state */
};

/*
 * Initialize the camera.
 */
void cam_init(struct timer_head *timers);

/*
 * Start the camera node.
 */
void cam_start(void);

/*
 * Stop the node simulator.
 */
void cam_stop(void);

/*
 * Camera node joining the network.
 * Return 0 on success or -1 on failure.
 */
int cam_node_add(enum camera_node_type type, unsigned sample_secs);

/*
 * The specified camera node leaving the network.
 * Return 0 on success or -1 on failure.
 */
int cam_node_remove(enum camera_node_type type);

/*
 * Handler called by the generic node management layer when the node is
 * being saved to non-volatile memory.  The JSON object returned is passed
 * to cam_node_load() to restore the node state.
 */
json_t *cam_node_save(const struct node *node);

#endif /* __NODE_CAMERA_H__ */
