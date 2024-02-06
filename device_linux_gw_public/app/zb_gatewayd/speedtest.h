/*
 * Copyright 2016-2018 Ayla Networks, Inc.  All rights reserved.
 *
 * Use of the accompanying software is permitted only in accordance
 * with and subject to the terms of the Software License Agreement
 * with Ayla Networks, Inc., a copy of which can be obtained from
 * Ayla Networks, Inc.
 */
#ifndef __SPEEDTEST_H__
#define __SPEEDTEST_H__

#define INTERNET_SPEED_CMD                      "ookla --configurl=http://www.speedtest.net/api/embed/k6plpslhp9uj12mo/config -f human-readable > /tmp/speedtest.txt"
#define UPLOAD_CMD                              "cat /tmp/speedtest.txt | grep Upload | awk '{print $2}'"
#define DOWNLOAD_CMD                            "cat /tmp/speedtest.txt | grep Download | awk '{print $2}'"
#define IS_UPLOAD_AVAIL_CMD                     "cat /tmp/speedtest.txt | grep Upload | awk '{print $1}'"
#define IS_DOWNLOAD_AVAIL_CMD                   "cat /tmp/speedtest.txt | grep Download | awk '{print $1}'"
#define IS_SPEEDTEST_INSTALLED	                "which ookla; echo $?"
#define INTERNET_SPEED_BUF_SIZE                 64
#define INTERNET_UPLOAD_DOWNLOAD_BUF_SIZE       32
#define CMD_BUF_SIZE                            256
#define UPLOAD_BUFFER                           "Upload:"
#define DOWNLOAD_BUFFER                         "Download:"

/*
 *To set the internet speed test enable:
 */
int appd_internet_speed_set(struct prop *prop, const void *val,
        size_t len, const struct op_args *args);

/*
 *Gateway internet Speed data update
 */
void appd_gw_internet_speed_update();

/*
 *Speedtest thread callback function to execute speedtest
 */
void* speed_test_thread_fun(void* arg);

/*
 *To get upload or download speed values
 */
bool get_upload_download_speed(char* value, bool is_upload);

#endif /* __SPEEDTEST_H__ */
