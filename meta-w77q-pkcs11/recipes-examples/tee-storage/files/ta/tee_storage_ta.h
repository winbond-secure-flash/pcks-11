/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       tee_storage_ta.h
* @brief      Shared TA/CA header for the TEE Storage utility
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: MIT */

#ifndef TEE_STORAGE_TA_H__
#define TEE_STORAGE_TA_H__

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/* TA UUID: a6f6e048-f6c4-4a0e-b629-4b4d9e8c1d5e */
#define TA_TEE_STORAGE_UUID \
    { 0xa6f6e048, 0xf6c4, 0x4a0e, \
      { 0xb6, 0x29, 0x4b, 0x4d, 0x9e, 0x8c, 0x1d, 0x5e } }

/* ── Named-object commands ────────────────────────────────────────────────
 *
 *  CMD_WRITE   param[0] MEMREF_INPUT  : object ID (string, not NUL-terminated)
 *              param[1] MEMREF_INPUT  : data payload
 *              (overwrites existing object)
 *
 *  CMD_READ    param[0] MEMREF_INPUT  : object ID
 *              param[1] MEMREF_INOUT  : output buffer; .size updated with
 *                                       actual bytes read on return
 *
 *  CMD_ERASE   param[0] MEMREF_INPUT  : object ID
 *
 *  CMD_LIST    param[0] MEMREF_OUTPUT : packed NUL-terminated IDs
 *                                       "id1\0id2\0id3\0"; .size = bytes written
 *              param[1] VALUE_OUTPUT  : .a = number of objects
 */
#define CMD_WRITE           0
#define CMD_READ            1
#define CMD_ERASE           2
#define CMD_LIST            3

/* ── Sector commands ──────────────────────────────────────────────────────
 *
 *  Sectors are stored as persistent objects with ID "S%08x" (9 ASCII bytes).
 *  addr is a 32-bit logical sector address; callers use any granularity.
 *
 *  CMD_WRITE_SECTOR  param[0] VALUE_INPUT  : .a = sector address (uint32)
 *                    param[1] MEMREF_INPUT : data (max TS_MAX_DATA_LEN)
 *
 *  CMD_READ_SECTOR   param[0] VALUE_INPUT  : .a = sector address
 *                    param[1] MEMREF_INOUT : output buffer; .size = actual bytes
 *
 *  CMD_ERASE_SECTOR  param[0] VALUE_INPUT  : .a = sector address
 */
#define CMD_WRITE_SECTOR    4
#define CMD_READ_SECTOR     5
#define CMD_ERASE_SECTOR    6

/* ── Storage-level commands ───────────────────────────────────────────────
 *
 *  CMD_FORMAT  no parameters — deletes every object in TEE_STORAGE_PRIVATE
 *
 *  CMD_INFO    param[0] VALUE_OUTPUT : .a = object count
 *              param[1] VALUE_OUTPUT : .a = total data bytes across all objects
 */
#define CMD_FORMAT          7
#define CMD_INFO            8

/* ── Limits ───────────────────────────────────────────────────────────── */
#define TS_MAX_ID_LEN       64      /* max named object ID length (bytes)  */
#define TS_MAX_DATA_LEN     4096    /* max single-object data payload      */
#define TS_MAX_LIST_BUF     8192    /* max output buffer for CMD_LIST      */
#define TS_SECTOR_SIZE      4096    /* canonical sector size for reference */

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // TEE_STORAGE_TA_H__
