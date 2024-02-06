/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */

/*
 * Ookla Speed test
 *
 *
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <ayla/log.h>
#include <ayla/assert.h>
#include <assert.h>
#include <ayla/build.h>
#include <ayla/utypes.h>
#include <ayla/http.h>
#include "libtransformer.h"
#include <inttypes.h>
#include <ayla/json_parser.h>
#include <regex.h>
#include <libgen.h>
#include <stdlib.h>
#include <dirent.h>

#include <ayla/ayla_interface.h>
#include <ayla/time_utils.h>
#include <ayla/timer.h>
#include <ayla/gateway_interface.h>
#include <app/app.h>
#include <app/ops.h>
#include <app/props.h>
#include <app/gateway.h>

#include "speedtest.h"
#include "pthread.h"


char gw_internet_download_speed[INTERNET_SPEED_BUF_SIZE];
char gw_internet_upload_speed[INTERNET_SPEED_BUF_SIZE];
char gw_speed_test_status[INTERNET_SPEED_BUF_SIZE];
u8 gw_speed_test_enable;
pthread_t speed_test_thread = (pthread_t)NULL;

/*
 *To get the internet speed:
 */
int appd_internet_speed_set(struct prop *prop, const void *val,
        size_t len, const struct op_args *args)
{
        if (prop_arg_set(prop, val, len, args) != ERR_OK) {
                log_err("prop_arg_set returned error");
                return -1;
        }
        if(gw_speed_test_enable)
        {
                if (!speed_test_thread) {
                        pthread_attr_t tattr;
                        size_t stacksize;
                        int detachstate;
                        int rc;
                        pthread_attr_init(&tattr);
                        size_t size = PTHREAD_STACK_MIN + 0x100;
                        pthread_attr_setstacksize(&tattr, size);
                        pthread_attr_setdetachstate(&tattr,PTHREAD_CREATE_DETACHED);
                        if (pthread_create(&speed_test_thread, &tattr, (void *)&speed_test_thread_fun, NULL)) {
                                log_debug("pthread creation failed");
                                pthread_cancel(speed_test_thread);
                        }
                        rc = pthread_attr_getdetachstate (&tattr, &detachstate);
                        log_info("detached state [%d] [%d]",detachstate,rc);
                        pthread_attr_getstacksize(&tattr, &stacksize);
                        log_info("speedtest thrd stack size: %u", stacksize);
                        pthread_attr_destroy(&tattr);
	        }
        }
        return 0;
}

/*
 *Gateway internet Speed data update
 */
void appd_gw_internet_speed_update()
{
        log_info("internet script called");
        remove("/tmp/speedtest.txt");
        memset(gw_speed_test_status,0x00,sizeof(gw_speed_test_status));
        strcpy(gw_speed_test_status,"speed_test_started");
        prop_send_by_name("gw_speed_test_status");
        memset(gw_internet_download_speed,0x00,sizeof(gw_internet_download_speed));
        memset(gw_internet_upload_speed,0x00,sizeof(gw_internet_upload_speed));
        strcpy(gw_internet_download_speed,"0");
        strcpy(gw_internet_upload_speed,"0");
        prop_send_by_name("gw_internet_download_speed");
        prop_send_by_name("gw_internet_upload_speed");
        system(INTERNET_SPEED_CMD);
        memset(gw_speed_test_status,0x00,sizeof(gw_speed_test_status));
        if(get_upload_download_speed(gw_internet_upload_speed,true)
                && get_upload_download_speed(gw_internet_download_speed,false))
        {
                prop_send_by_name("gw_internet_download_speed");
                prop_send_by_name("gw_internet_upload_speed");
                strcpy(gw_speed_test_status,"speed_test_completed");
        }else{
                strcpy(gw_speed_test_status,"speed_test_failed");
        }
        prop_send_by_name("gw_speed_test_status");
        gw_speed_test_enable = 0;
        prop_send_by_name("gw_speed_test_enable");
}

/*
 *Speedtest thread callback function to execute speedtest
 */
void* speed_test_thread_fun(void* arg)
{
        log_debug("speed_test_thread_fun, the thread id = %lu", pthread_self());
        appd_gw_internet_speed_update();
        speed_test_thread = (pthread_t)NULL;
        pthread_exit(0);
}

/*
 *To get upload or download speed values
 */
bool get_upload_download_speed(char* value, bool is_upload)
{
        FILE *cmd_fp=NULL;
        char buffer[CMD_BUF_SIZE] = {0};
        char speed_value[INTERNET_UPLOAD_DOWNLOAD_BUF_SIZE]={0};
        bool ret_val = false;

        cmd_fp = popen((is_upload)?IS_UPLOAD_AVAIL_CMD:IS_DOWNLOAD_AVAIL_CMD,"r");
        if(cmd_fp)
        {
                fscanf(cmd_fp, "%s", buffer);
                pclose(cmd_fp);
        }
        if(!strcmp(buffer,(is_upload)?UPLOAD_BUFFER:DOWNLOAD_BUFFER))
        {
                cmd_fp = popen((is_upload)?UPLOAD_CMD:DOWNLOAD_CMD,"r");
                if(cmd_fp)
                {
                        fscanf(cmd_fp, "%s", speed_value);
                        pclose(cmd_fp);
                        if(strcmp(speed_value,"FAILED"))
                        {
                                memset(value,0x00,strlen(value));
                                sprintf(value,"%s Mbps",speed_value);
                                ret_val = true;
                        }
                }
        }
        return ret_val;
}