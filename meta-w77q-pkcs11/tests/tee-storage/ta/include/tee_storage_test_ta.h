/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * tee_storage_test_ta.h — Generic TEE secure storage test TA interface.
 *
 * TA UUID: c3d4e5f6-a7b8-90ab-cdef-012345678901
 *
 * Exposes a simple key→value store over TEE_STORAGE_PRIVATE so a Python
 * host app can exercise every GP TEE storage code path without a compiled
 * C host binary.
 *
 * Each command uses the following parameter conventions:
 *   p[0] = VALUE_INOUT  — flags / return info (command-specific)
 *   p[1] = MEMREF_INPUT — key bytes  (object ID)
 *   p[2] = MEMREF_INPUT or MEMREF_OUTPUT — value / output buffer
 *   p[3] = NONE
 *
 * See individual command descriptions below.
 */
#ifndef TEE_STORAGE_TEST_TA_H
#define TEE_STORAGE_TEST_TA_H

/* ---- UUID ---------------------------------------------------------------- */
#define TEE_STORAGE_TEST_TA_UUID \
	{ 0xc3d4e5f6, 0xa7b8, 0x90ab, \
	  { 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0x01 } }

/* ---- Commands ------------------------------------------------------------ */

/*
 * CMD_WRITE (0) — create or overwrite a persistent object.
 *   p[0] = NONE
 *   p[1] = MEMREF_INPUT  key bytes
 *   p[2] = MEMREF_INPUT  value bytes (may be empty)
 *   p[3] = NONE
 *   Returns TEE_SUCCESS or TEE error.
 */
#define TST_CMD_WRITE     0U

/*
 * CMD_READ (1) — read value for a key.
 *   p[0] = VALUE_OUTPUT  p[0].value.a = actual data size on return
 *   p[1] = MEMREF_INPUT  key bytes
 *   p[2] = MEMREF_OUTPUT output buffer; if too small returns
 *                        TEE_ERROR_SHORT_BUFFER and sets p[0].value.a
 *   p[3] = NONE
 *   Returns TEE_SUCCESS, TEE_ERROR_ITEM_NOT_FOUND, or TEE_ERROR_SHORT_BUFFER.
 */
#define TST_CMD_READ      1U

/*
 * CMD_DELETE (2) — delete a persistent object.
 *   p[0] = NONE
 *   p[1] = MEMREF_INPUT  key bytes
 *   p[2] = NONE
 *   p[3] = NONE
 *   Returns TEE_SUCCESS or TEE_ERROR_ITEM_NOT_FOUND.
 */
#define TST_CMD_DELETE    2U

/*
 * CMD_EXISTS (3) — check whether an object exists.
 *   p[0] = VALUE_OUTPUT  p[0].value.a = 1 if exists, 0 if not
 *                        p[0].value.b = data size (if exists)
 *   p[1] = MEMREF_INPUT  key bytes
 *   p[2] = NONE
 *   p[3] = NONE
 *   Always returns TEE_SUCCESS.
 */
#define TST_CMD_EXISTS    3U

/*
 * CMD_LIST (4) — enumerate all objects owned by this TA.
 *   p[0] = VALUE_OUTPUT  p[0].value.a = number of objects found
 *   p[1] = MEMREF_OUTPUT NUL-separated object IDs; if too small only
 *                        what fits is written (p[0].value.a still correct)
 *   p[2] = NONE
 *   p[3] = NONE
 *   Always returns TEE_SUCCESS.
 */
#define TST_CMD_LIST      4U

/*
 * CMD_GET_SIZE (5) — return the data size of an object.
 *   p[0] = VALUE_OUTPUT  p[0].value.a = data size in bytes
 *   p[1] = MEMREF_INPUT  key bytes
 *   p[2] = NONE
 *   p[3] = NONE
 *   Returns TEE_SUCCESS or TEE_ERROR_ITEM_NOT_FOUND.
 */
#define TST_CMD_GET_SIZE  5U

/*
 * CMD_CLEAR_ALL (6) — delete every object owned by this TA.
 *   p[0] = VALUE_OUTPUT  p[0].value.a = number of objects deleted
 *   p[1] = NONE
 *   p[2] = NONE
 *   p[3] = NONE
 *   Always returns TEE_SUCCESS.
 */
#define TST_CMD_CLEAR_ALL 6U

/* ---- Limits -------------------------------------------------------------- */
#define TST_MAX_KEY_LEN   128U   /* maximum object ID length in bytes */
#define TST_MAX_VAL_LEN   65536U /* maximum value size (64 KB)        */

#endif /* TEE_STORAGE_TEST_TA_H */
