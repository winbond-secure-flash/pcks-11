/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       user_ta_header_defines.h
* @brief      OP-TEE TA metadata for the TEE Storage TA
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

#include "tee_storage_ta.h"

/* UUID must match the BINARY name in the Makefile */
#define TA_UUID     TA_TEE_STORAGE_UUID

/* TA_FLAG_EXEC_DDR: TA image loaded into DRAM */
#define TA_FLAGS    TA_FLAG_EXEC_DDR

/*
 * Stack: 8 KB — large enough for the format loop and enumerator operations.
 * Data:  64 KB — comfortable headroom for list output buffer (8 KB) and any
 *                internal allocations in the OP-TEE storage layer.
 */
#define TA_STACK_SIZE   (8  * 1024)
#define TA_DATA_SIZE    (64 * 1024)

#define TA_VERSION      "1.0"
#define TA_DESCRIPTION  "TEE Storage: read/write/erase/format via W77Q backend"

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // USER_TA_HEADER_DEFINES_H__
