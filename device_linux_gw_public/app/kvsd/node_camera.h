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
	bool started;					/* Streaming started */
	pid_t pid;						/* Streaming process ID */
	struct timer stream_timer;		/* Timer for streaming */
	struct timer start_delay_timer;	/* Timer for streaming start delay */
	void* stream_data;				/* Streaming data */
	struct timer stream_update_timer;	/* Timer for stream updates */
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

	struct kvs_data kvs_data;	/* KVS data for node */
	struct stream_state kvs_stream_state;	/* KVS streaming state */

	struct webrtc_data webrtc_data;	/* WebRTC data for node */
	struct stream_state webrtc_stream_state;	/* WebRTC streaming state */
};

/*
 * Initialize the camera.
 */
void cam_init(struct timer_head *timers);

/*
 * Start the camera node.  @TODO: Change this: This begins to periodically update
 * simulated environmental conditions to use for generating node datapoints.
 */
void cam_start(void);

/*
 * Stop the node simulator.
 */
void cam_stop(void);

/*
 * Camera node joining the network. @TODO: Change this: Sample_secs is optional and the
 * default value will be used if it is set to 0.
 * Return 0 on success or -1 on failure.
 */
int cam_node_add(enum camera_node_type type, unsigned sample_secs);

/*
 * The specified camera node leaving the network. @TODO: Change this:
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
