/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __NODE_SIM_H__
#define __NODE_SIM_H__

/*
 * Types of virtual nodes supported by the simulator.
 */
#define SIM_NODE_TYPES(def)				\
	def(thermostat,		SIM_THERMOSTAT)		\
	def(sensor,		SIM_SENSOR)		\
							\
	def(,			SIM_TYPE_COUNT)	/* MUST be last entry */

DEF_ENUM(sim_node_type, SIM_NODE_TYPES);

/*
 * Initialize the node simulator.
 */
void sim_init(struct timer_head *timers);

/*
 * Start the node simulator.  This begins to periodically update
 * simulated environmental conditions to use for generating node datapoints.
 */
void sim_start(void);

/*
 * Stop the node simulator.
 */
void sim_stop(void);

/*
 * Simulate a node joining the network.  Sample_secs is optional and the
 * default value will be used if it is set to 0.
 * Return 0 on success or -1 on failure.
 */
int sim_node_add(enum sim_node_type type, unsigned sample_secs);

/*
 * Simulate the specified node leaving the network.
 * Return 0 on success or -1 on failure.
 */
int sim_node_remove(enum sim_node_type type);

#endif /* __NODE_SIM_H__ */
