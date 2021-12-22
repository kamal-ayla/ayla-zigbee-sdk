/*
 * Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __AYLA_BUILD_H__
#define __AYLA_BUILD_H__

/*
 * make/common_defs.mk defines the following pre-processor variables:
 *   BUILD_VERSION
 *   BUILD_RELEASE
 *   BUILD_SCM_REV
 *   BUILD_DATE
 *   BUILD_TIME
 *   BUILD_USER
 */

/*
 * Create a formatted build version string for convenience.
 */
#ifdef BUILD_RELEASE
#define BUILD_VERSION_LABEL \
    BUILD_VERSION " " BUILD_DATE " " BUILD_TIME
#else
#define BUILD_VERSION_LABEL \
    BUILD_VERSION " " BUILD_DATE " " BUILD_TIME " " BUILD_USER "/" BUILD_SCM_REV
#endif

#endif /* __AYLA_BUILD_H__ */
