/** @file check.h
  * @author Maciej Andrzejewski
  * @date 07.08.23
  * @copyright Copyright © M-Works, Maciej Andrzejewski 2023
  * @brief A Documented file.
  * @details Details.
  */

#ifndef __CHECK_H__
#define __CHECK_H__

#define CHK_RET(x) if(0 != x) { return x; }
#define CHK_PTR(x) if(NULL == x) { return -1; }

#endif // __CHECK_H__
