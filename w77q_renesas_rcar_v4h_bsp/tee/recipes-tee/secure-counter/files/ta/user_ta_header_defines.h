/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       user_ta_header_defines.h
* @brief      OP-TEE TA metadata for the secure counter TA
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: MIT */

#ifndef USER_TA_HEADER_DEFINES_H__
#define USER_TA_HEADER_DEFINES_H__

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include "secure_counter_ta.h"

/* UUID — must match the BINARY filename in the Makefile */
#define TA_UUID     TA_SECURE_COUNTER_UUID

/* TA_FLAG_EXEC_DDR: TA image loaded into DRAM (normal for most TAs) */
#define TA_FLAGS    TA_FLAG_EXEC_DDR

/* Stack / heap sizes (bytes) — keep small for a simple TA */
#define TA_STACK_SIZE   (2 * 1024)
#define TA_DATA_SIZE    (32 * 1024)

#define TA_VERSION      "1.0"
#define TA_DESCRIPTION  "Secure Counter Trusted Application"

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // USER_TA_HEADER_DEFINES_H__
