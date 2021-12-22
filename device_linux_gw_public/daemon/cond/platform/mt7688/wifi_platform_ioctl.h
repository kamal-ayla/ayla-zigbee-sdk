#ifndef __WIFI_PLATFORM_IOCTL_H__
#define __WIFI_PLATFORM_IOCTL_H__

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
extern int wifi_platform_get_scan_result(const char *ifname,
	char **scan_buf, int *scan_buf_len);

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
extern int wifi_platform_get_connstatus(const char *ifname,
	int *status_val);

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
extern int wifi_platform_ioctl_set_net(const char *ifname,
	const char *cmd, const char *val, size_t val_len);

#endif /* __WIFI_PLATFORM_IOCTL_H__ */
