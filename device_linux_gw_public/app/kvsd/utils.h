/** @file utils.h
  * @author Maciej Andrzejewski
  * @date 18.05.23
  * @copyright Copyright © M-Works, Maciej Andrzejewski 2023
  * @brief A Documented file.
  */

#ifndef UTILS_H
#define UTILS_H

#include <ayla/utypes.h>

#define ip_str_container(name)  char name[16]

/**
 * @brief Check if IP string is valid.
 * @param ipString IP string to check.
 * @return True if IP string is valid, false otherwise.
 */
bool ipstr_is_valid(const char* ipString);

/**
 * @brief Convert IP string to U32.
 * @param ipString IP string to convert.
 * @param ip U32 to store result.
 * @return  0 if success, !=0 otherwise.
 */
int ipstr_to_u32(const char* ipString, u32* ip);

/**
 * @brief Convert U32 to IP string.
 * @param ip U32 to convert.
 * @param ipString IP string to store result. Must be at least 16 bytes long.
 */
void u32_to_ipstr(u32 ip, char* ipString);

int get_url_userpass(const char* url, const char* username, const char* passwd, char* output);

int check_url_userpass(const char* url);

#endif // UTILS_H
