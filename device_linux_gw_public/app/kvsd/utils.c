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

#include "utils.h"


bool ipstr_is_valid(const char* ipString)
{
    if (NULL == ipString) {
        return false;
    }

    int count = 0;
    int num = 0;

    // Iterate through each character in the string
    for (int i = 0; ipString[i] != '\0'; ++i) {
        if (ipString[i] == ':') {       // @TODO: MAN: Changed the separator from '.' to ':' because server backend does not support '.' in string. To be changed back to '.' when backend is updated.
            // Check if the number of characters between dots is valid
            if (count < 1 || count > 3) {
                return false;
            }

            // Reset the count for the next octet
            count = 0;

            // Check if the number is in the valid range
            if (num < 0 || num > 255) {
                return false;
            }

            // Reset the number for the next octet
            num = 0;
        }
        else if (ipString[i] >= '0' && ipString[i] <= '9') {
            // Accumulate the number
            num = num * 10 + (ipString[i] - '0');

            // Check if the accumulated number is too large
            if (num > 255) {
                return false;
            }

            // Increment the count of characters
            ++count;
        }
        else {
            // Invalid character found
            return false;
        }
    }

    // Check the last octet
    if (count < 1 || count > 3 || num < 0 || num > 255) {
        return false;
    }

    return true;
}

int ipstr_to_u32(const char* ipString, u32* ip)
{
    if (NULL == ip || (! ipstr_is_valid(ipString))) {
        return -1;
    }

    int shift = 24;

    char* token = strtok((char*)ipString, ":");     // @TODO: MAN: Changed the separator from '.' to ':' because server backend does not support '.' in string. To be changed back to '.' when backend is updated.
    while (token != NULL && shift >= 0) {
        unsigned int octet = strtol(token, NULL, 10);
        *ip |= octet << shift;
        shift -= 8;
        token = strtok(NULL, ":");      // @TODO: MAN: Changed the separator from '.' to ':' because server backend does not support '.' in string. To be changed back to '.' when backend is updated.
    }

    return 0;
}

void u32_to_ipstr(unsigned int ip, char* ipString)
{
    if (NULL == ipString) {
        return;
    }

    snprintf(ipString, 16, "%u:%u:%u:%u",       // @TODO: MAN: Changed the separator from '.' to ':' because server backend does not support '.' in string. To be changed back to '.' when backend is updated.
            (ip >> 24) & 0xFF,
            (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF,
            ip & 0xFF);
}

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
