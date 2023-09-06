/** @file utils.c
  * @author Maciej Andrzejewski
  * @date 18.05.23
  * @copyright Copyright © M-Works, Maciej Andrzejewski 2023
  * @brief A Documented file.
  * @details Details.
  */

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include <ayla/log.h>
#include <signal.h>
#include <unistd.h>

#include "utils.h"


int check_url_userpass(const char* url)
{
	const char* protocol_delimiter = "://";
	const char* userpass_start = strstr(url, protocol_delimiter);
	if (userpass_start == NULL) {
		return -1;	// username and password missing
	}

	/* Skip the protocol delimiter */
	userpass_start += strlen(protocol_delimiter);

	const char* userpass_end = strchr(userpass_start, '@');
	if (userpass_end == NULL) {
		return -1;	// username and password missing
	}

	const char* username_start = userpass_start;
	const char* password_start = strchr(userpass_start, ':');

	if (password_start == NULL || password_start > userpass_end) {
		return 1;	// PASSWORD_MISSING
	}

	if (username_start == password_start) {
		return 2;	// USERNAME_MISSING
	}

	return 0;	// username and password are present
}

int get_url_userpass(const char* url, const char* username, const char* passwd, char* output)
{
	const char* protocol_delimiter = "://";
	const char* host_start = strstr(url, protocol_delimiter);
	if (host_start == NULL)
		return -1;

	/* Calculate the position to insert username and password */
	size_t protocol_delimiter_len = strlen(protocol_delimiter);
	size_t insert_pos = host_start - url + protocol_delimiter_len;

	/* Copy the URL up to the insertion position */
	strncpy(output, url, insert_pos);
	output[insert_pos] = '\0';

	/* Append the username and password */
	if(strlen(username) > 0) {
		strcat(output, username);
		if (strlen(passwd) > 0) {
			strcat(output, ":");
			strcat(output, passwd);
		}
		strcat(output, "@");
	}

	/* Append the rest of the URL after the insertion position */
	strcat(output, host_start + protocol_delimiter_len);

	return 0;
}

int kill_proc(pid_t pid, uint32_t wait_ms)
{
	if (kill(pid, SIGTERM) != 0) {
		log_err("Failed to terminate process: %d", pid);
		return -1;
	}

	usleep(wait_ms * 1000);

	if (kill(pid, SIGKILL) != 0) {
		log_err("Failed to kill process: %d", pid);
		return -1;
	}

	return 0;
}

int kill_all_proc(const char* proc_name, uint32_t wait_ms)
{
	FILE *fp;
	char pid[32];
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "pgrep -f %s", proc_name);

	// Execute the pgrep command to get the PIDs of all processes named "process_name"
	fp = popen(cmd, "r");
	if (fp == NULL) {
		log_err("Failed to run command");
		return -1;
	}

	// Read the output, which contains the PIDs of processes named "process_name"
	while (fgets(pid, sizeof(pid)-1, fp) != NULL) {
		int pid_val = atoi(pid);
		// Kill each process
		if (kill(pid_val, SIGTERM) == -1) {
			log_err("Failed to terminate process: %d", pid_val);
		}
	}

	// Close the file pointer
	pclose(fp);

	usleep(wait_ms * 1000);

	// Repeat and SIGKILL if process still exists
	fp = popen(cmd, "r");
	if (fp == NULL) {
		log_err("Failed to run command");
		return -1;
	}

	int err = 0;
	while (fgets(pid, sizeof(pid)-1, fp) != NULL) {
		int pid_val = atoi(pid);
		// Kill each process
		if (kill(pid_val, SIGKILL) == -1) {
			log_err("Failed to kill process: %d", pid_val);
			++err;
		}
	}

	return err;
}
