/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_APPD_H__
#define __AYLA_APPD_H__

#include <app/app.h>

/* Application software version */
extern const char *appd_version;


/*
 * Hook for the app library to initialize the user-defined application.
 */
int appd_init(void);

/*
 * Hook for the app library to start the user-defined application.  Once
 * This function returns, the app library will enable receiving updates from
 * the cloud, and begin to process tasks on the main thread.
 */
int appd_start(void);

/*
 * Hook for the app library to notify the user-defined application that the
 * process is about to terminate.
 */
void appd_exit(int status);

/*
 * Function called during each main loop iteration.  This may be used to
 * perform routine tasks that are not linked to a specific timer or file event.
 */
void appd_poll(void);

/*
 * Hook for the app library to notify the user-defined application that a
 * factory reset is about to occur.
 */
void appd_factory_reset(void);

/*
 * Hook for the app library to notify the user-defined application that the
 * the connectivity status has changed.
 */
void appd_connectivity_event(enum app_conn_type type, bool up);

/*
 * Hook for the app library to notify the user-defined application that the
 * the user registration status has changed.
 */
void appd_registration_event(bool registered);

#endif /* __AYLA_APPD_H__ */
