/***************************************************************************/ /**
 * @file
 * @brief CPC system endpoint common
 *******************************************************************************
 * # License
 * <b>Copyright 2019 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/

#ifndef SL_CPC_SECURITY_COMMON_H_
#define SL_CPC_SECURITY_COMMON_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/***************************************************************************/ /**
 * @addtogroup cpc_security_secondary
 * @brief CPC Security Secondary
 * @details
 * @{
 ******************************************************************************/

/// The security state enabled bit mask
#define SL_CPC_SECURITY_STATE_ENABLE_MASK (1 << 0)

/// The security state bounded bit mask
#define SL_CPC_SECURITY_STATE_BOUND_MASK  (1 << 1)

/** @} (end addtogroup cpc_security_secondary) */

#ifdef __cplusplus
}
#endif

#endif /* SL_CPC_SECURITY_COMMON_H_ */
