#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ayla/utypes.h>
#include <ayla/log.h>

#include <linux/wireless.h>

#include "wifi_platform_ioctl.h"

#if WIRELESS_EXT <= 11
#ifndef SIOCDEVPRIVATE
#define SIOCDEVPRIVATE 0x8BE0
#endif
#define SIOCIWFIRSTPRIV SIOCDEVPRIVATE
#endif

#define RTPRIV_IOCTL_SET (SIOCIWFIRSTPRIV + 0x02)
#define RTPRIV_IOCTL_GSITESURVEY (SIOCIWFIRSTPRIV + 0x0D)
#define RTPRIV_IOCTL_SHOW (SIOCIWFIRSTPRIV + 0x11)

#define BUF_256 256
#define BUF_8192 8192

/*
  * send ioctl to set wifi driver
  * Input params:
  *     ifname: wifi interface name
  *     ioctl_type: ioctl type
  *     buf: ioctl data
  *     buf_size: length of ioctl data
  * Output params:
  *     out_len: output data length after ioctl calling
  * Return value:
  *     0: success,  -1: fail
  */
static int wifi_platform_ioctl_send(const char *ifname,
	uint16_t ioctl_type, char *buf, size_t buf_size, size_t *out_len)
{
	int socket_id;
	struct iwreq wrq;
	int copy_len;
	int ret;

	socket_id = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_id < 0) {
		log_err("open socket error");
		return -1;
	}

	copy_len = sizeof(wrq.ifr_name) - 1;
	strncpy(wrq.ifr_name, ifname, copy_len);
	wrq.ifr_name[copy_len] = 0;
	wrq.u.data.length = buf_size;
	wrq.u.data.pointer = buf;
	wrq.u.data.flags = 0;
	ret = ioctl(socket_id, ioctl_type, &wrq);
	if (ret) {
		log_err("ioctl error");
		close(socket_id);
		return ret;
	}

	if (out_len) {
		*out_len = wrq.u.data.length;
	}

	close(socket_id);
	return 0;
}

/*
  * Get scan result from wifi driver
  * Input Params:
  *     ifname: wifi interface name
  * Output Params:
  *     scan_buf: buffer that contain scan results
  *         should be freed by the calling function
  *     scan_buf_len: scan results buffer length
  * Return value:
  *     0: success,  -1: fail
  */
int wifi_platform_get_scan_result(const char *ifname,
	char **scan_buf, int *scan_buf_len)
{
	int ret;
	char *data;
	size_t out_len;

	*scan_buf = NULL;
	*scan_buf_len = 0;

	log_debug("get scan result");

	data = calloc(1, BUF_8192);
	if (data == NULL) {
		log_err("failed to allocate memory");
		return -1;
	}

	ret = wifi_platform_ioctl_send(ifname, RTPRIV_IOCTL_GSITESURVEY,
		data, BUF_8192, &out_len);
	if (ret) {
		log_err("get site survey ioctl error");
		free(data);
		return -1;
	}

	log_debug("Get Site Survey AP List, data length:%zu",
		out_len);

	*(data + out_len) = 0;
	*scan_buf = data;
	*scan_buf_len = out_len;

	return 0;
}

/*
  * Get connection status of wifi interface
  *     that connect to selected AP in STA mode
  * Input Params:
  *     ifname: wifi interface name
  * Output Params:
  *     status_val: connect status
  *         1: connected, 0: disconnected
  * Return value:
  *     0: success,  -1: fail
  */
int wifi_platform_get_connstatus(const char *ifname,
	int *status_val)
{
	char buffer[BUF_256];
	int ret;
	char *status_str;
	size_t out_len;

	snprintf(buffer, sizeof(buffer), "connStatus=0");

	ret = wifi_platform_ioctl_send(ifname, RTPRIV_IOCTL_SHOW,
		buffer, strlen(buffer) + 1, &out_len);
	if (ret) {
		log_err("get connect status ioctl error");
		return -1;
	}

	/* buffer will be overwrite when ioctl success,
	    '=' will be set to '\0',
	    '0' will be set to real value of
	    connect status ( '0' on disconnection, '1' on connection) */
	status_str = buffer + strlen(buffer) + 1;
	sscanf(status_str, "%d", status_val);
	log_debug("wifi platform get connect status length:%zu,"
		" value:%s val:%d",
		out_len, status_str, *status_val);

	return 0;
}

/*
  * Set network params of wifi interface
  * Input Params:
  *     ifname: wifi interface name
  *     cmd: network command name
  *     val: value of network command
  *     val_len: length of value
  * Return value:
  *     0: success,  -1: fail
  */
int wifi_platform_ioctl_set_net(const char *ifname,
	const char *cmd, const char *val, size_t val_len)
{
	char buffer[BUF_256];
	size_t len;
	int ret;

	log_debug("ioctl set cmd:%s, val:%s, val_len:%zu",
		cmd, val, val_len);

	len = snprintf(buffer, sizeof(buffer), "%s=", cmd);
	memcpy(buffer + len, val, val_len);
	*(buffer + len + val_len) = 0;

	ret = wifi_platform_ioctl_send(ifname, RTPRIV_IOCTL_SET,
		buffer, len + val_len + 1, NULL);
	if (ret) {
		log_err("set net ioctl error");
		return -1;
	}

	return 0;
}
