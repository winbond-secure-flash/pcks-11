/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_dump_ta.h
* @brief      Shared UUID and command definitions for the W77Q flash dump Pseudo-TA
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef W77Q_DUMP_TA_H__
#define W77Q_DUMP_TA_H__

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include <stdint.h>

/*
 * W77Q dump Pseudo-TA UUID:
 *   3e8a1b2c-4f5d-6e7a-8b9c-0d1e2f3a4b5c
 */
#define W77Q_DUMP_TA_UUID \
    { 0x3e8a1b2cU, 0x4f5dU, 0x6e7aU, \
      { 0x8bU, 0x9cU, 0x0dU, 0x1eU, 0x2fU, 0x3aU, 0x4bU, 0x5cU } }

/*
 * W77Q_DUMP_CMD_LIST_ALL — Return all entries in the w77q_fs LUT.
 *
 * params[0]: MEMREF_INOUT  output buffer of w77q_dump_entry[]
 *            .size updated to bytes written on return
 * params[1]: VALUE_OUTPUT  .a = number of entries written
 *
 * Returns TEEC_SUCCESS.
 */
#define W77Q_DUMP_CMD_LIST_ALL  0U

/*
 * W77Q_DUMP_CMD_READ_RAW — Read raw bytes from W77Q flash.
 *
 * params[0]: VALUE_INPUT   .a = byte offset into flash
 * params[1]: MEMREF_INOUT  buffer; .size = bytes to read / actual on return
 *
 * Returns TEEC_SUCCESS on success.
 */
#define W77Q_DUMP_CMD_READ_RAW  1U

/*
 * W77Q_DUMP_CMD_WRITE_RAW — Write raw bytes to W77Q flash.
 *
 * Bypasses the FS layer — no header written, LUT not updated.
 * Diagnostic use only.
 *
 * params[0]: VALUE_INPUT   .a = byte offset into flash
 * params[1]: MEMREF_INPUT  data to write (.size = byte count, max 65536)
 *
 * Returns TEEC_SUCCESS on success.
 */
#define W77Q_DUMP_CMD_WRITE_RAW  2U

/*
 * W77Q_DUMP_CMD_ERASE_SECTOR — Erase the 4 KB sector containing the offset.
 *
 * Invalidates any FS records in the sector; LUT is rebuilt from flash.
 *
 * params[0]: VALUE_INPUT   .a = byte offset of (or within) the sector
 * params[1]: NONE
 *
 * Returns TEEC_SUCCESS on success.
 */
#define W77Q_DUMP_CMD_ERASE_SECTOR  3U

/*
 * W77Q_DUMP_CMD_ERASE_CHIP — Erase all of W77Q section 1.
 *
 * Destroys ALL stored objects and reinitialises the LUT (empty).
 * No parameters.
 *
 * Returns TEEC_SUCCESS on success.
 */
#define W77Q_DUMP_CMD_ERASE_CHIP  4U

/*
 * W77Q_DUMP_CMD_READ_PLAIN — Attempt to read flash WITHOUT authenticated session.
 *
 * Demonstrates that the W77Q hardware rejects unauthenticated reads when
 * the section is configured with plainAccessReadEnable=0.
 *
 * params[0]: VALUE_INPUT   .a = byte offset into flash
 * params[1]: MEMREF_INOUT  buffer; .size = bytes to read / actual on return
 *
 * Returns TEEC_SUCCESS if (surprisingly) plain read worked,
 *         or an error code showing the hardware rejected it.
 */
#define W77Q_DUMP_CMD_READ_PLAIN  5U

/*
 * W77Q_DUMP_CMD_READ_FILE — Read a file's content through LittleFS.
 *
 * Reads the object identified by (ta_uuid, obj_id) through the LFS filesystem
 * layer, returning the actual stored content.  Used when flash_off is not
 * meaningful (LFS abstracts physical layout).
 *
 * params[0]: MEMREF_INPUT   ta_uuid (16 bytes) || obj_id (variable, up to 64 bytes)
 * params[1]: MEMREF_INOUT   output buffer; .size = max bytes / actual on return
 *
 * Returns TEEC_SUCCESS on success.
 */
#define W77Q_DUMP_CMD_READ_FILE  6U

/* Maximum object-ID string length (matches W77Q_OBJ_ID_MAX = 64) */
#define W77Q_DUMP_OBJ_ID_MAX    64U

/*
 * One LUT entry as returned by W77Q_DUMP_CMD_LIST_ALL.
 * Packed to 92 bytes: 16 + 4 + 4 + 4 + 64.
 */
struct w77q_dump_entry
{
    uint8_t  ta_uuid_u8[16];             /* TA UUID of the owning TA       */
    uint32_t flash_off_u32;               /* byte offset in W77Q flash      */
    uint32_t data_size_u32;               /* payload bytes (excl. header)   */
    uint32_t obj_id_len_u32;              /* valid bytes in obj_id[]        */
    uint8_t  obj_id[W77Q_DUMP_OBJ_ID_MAX]; /* raw object ID (may be binary) */
} __attribute__((packed));

/* Maximum entries in one LIST_ALL call (= W77Q_FS_MAX_LUT = 244) */
#define W77Q_DUMP_MAX_ENTRIES   244U

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // W77Q_DUMP_TA_H__
