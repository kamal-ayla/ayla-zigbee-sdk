/** @file utils.h
  * @author Maciej Andrzejewski
  * @date 18.05.23
  * @copyright Copyright © M-Works, Maciej Andrzejewski 2023
  * @brief A Documented file.
  */

#ifndef UTILS_H
#define UTILS_H

#include <ayla/utypes.h>

int get_url_userpass(const char* url, const char* username, const char* passwd, char* output);
int check_url_userpass(const char* url);
int kill_all_proc(const char* proc_name, uint32_t wait_ms);
int kill_proc(pid_t pid, uint32_t wait_ms);

#endif // UTILS_H
