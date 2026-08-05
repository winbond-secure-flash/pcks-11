/******************************************************************************
 * @internal
 * @remark Winbond - Confidential
 * @copyright 2026 by Winbond Electronics Corporation. All rights reserved.
 * @file tee_storage_ta.c
 * @brief TEE Storage Trusted Application (Secure World) — persistent-object
 *        read/write/erase/list and sector-addressed read/write/erase/format
 *        commands backed by TEE_STORAGE_PRIVATE.
 *
 * ### project meta-w77q-pkcs11
 *
 * SPDX-License-Identifier: MIT
 *
 * Object ID namespacing
 * ---------------------
 *   Named objects  : user-supplied ASCII string  (e.g. "config", "key1")
 *   Sector objects : "S" + 8 lower-case hex digits (e.g. "S00001000")
 *                    so sector 0x1000 -> object ID "S00001000" (9 bytes).
 *   The two spaces collide only if a user names an object "S" followed by
 *   exactly 8 hex digits -- acceptable for a test/diagnostic tool.
 ******************************************************************************/

/*-----------------------------------------------------------------------------------------------------------
                                                   INCLUDES
-----------------------------------------------------------------------------------------------------------*/

#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <stdio.h>

#include "tee_storage_ta.h"

/* ── TA lifecycle ────────────────────────────────────────────────────────── */

TEE_Result TA_CreateEntryPoint(void)
{
    DMSG("tee_storage: TA_CreateEntryPoint");
    return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
    DMSG("tee_storage: TA_DestroyEntryPoint");
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types_u32 __unused,
                                    TEE_Param params[4] __unused,
                                    void **sess_ctx_pv __unused)
{
    DMSG("tee_storage: session opened");
    return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *sess_ctx_pv __unused)
{
    DMSG("tee_storage: session closed");
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Build a 9-byte ASCII sector object ID from a 32-bit address. */
static void make_sector_id_L(uint32_t addr_u32, char id_pv[10])
{
    snprintf(id_pv, 10u, "S%08x", addr_u32);
}

/* Open a persistent object for read; returns handle or sets *res. */
static TEE_Result open_for_read_L(const void *id_pv, size_t id_len_u32,
                                TEE_ObjectHandle *out)
{
    return TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
                                   id_pv, id_len_u32,
                                   TEE_DATA_FLAG_ACCESS_READ,
                                   out);
}

/*
 * Open a persistent object for deletion.
 * GP spec §5.7.3: TEE_CloseAndDeletePersistentObject1 requires the handle to
 * have been opened with TEE_DATA_FLAG_ACCESS_WRITE_META (0x4).  On OP-TEE 4.5
 * + W77Q backend, opening with ACCESS_WRITE only → ACCESS_DENIED panic, and
 * opening without WRITE_META → TEE_CloseAndDeletePersistentObject1 returns
 * TEE_ERROR_ACCESS_CONFLICT which libutee escalates to a TA panic.
 */
static TEE_Result open_for_delete_L(const void *id_pv, size_t id_len_u32,
                                  TEE_ObjectHandle *out)
{
    return TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
                                   id_pv, id_len_u32,
                                   TEE_DATA_FLAG_ACCESS_WRITE_META,
                                   out);
}

/* ── cmd_write_L ───────────────────────────────────────────────────────────── */

static TEE_Result cmd_write_L(uint32_t param_types_u32, TEE_Param params[4])
{
    const uint32_t exp_u32 = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                                         TEE_PARAM_TYPE_MEMREF_INPUT,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE);
    if (param_types_u32 != exp_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    const void  *id_ptr_pv   = params[0].memref.buffer;
    uint32_t     id_len_u32   = (uint32_t)params[0].memref.size;
    const void  *data_pv     = params[1].memref.buffer;
    size_t       data_len = params[1].memref.size;

    if (!id_len_u32 || id_len_u32 > TS_MAX_ID_LEN)
        return TEE_ERROR_BAD_PARAMETERS;
    if (data_len > TS_MAX_DATA_LEN)
        return TEE_ERROR_EXCESS_DATA;

    /*
     * OP-TEE 4.5 storage syscall requires the objectID to be in TA-private
     * memory.  Passing params[].memref.buffer (shared memory) directly causes
     * TEE_ERROR_ACCESS_DENIED → TA panic.  Copy to a local stack buffer first.
     */
    uint8_t id_u8[TS_MAX_ID_LEN];
    TEE_MemMove(id_u8, id_ptr_pv, id_len_u32);

    TEE_ObjectHandle obj = TEE_HANDLE_NULL;
    TEE_Result res = TEE_CreatePersistentObject(
                         TEE_STORAGE_PRIVATE,
                         id_u8, id_len_u32,
                         TEE_DATA_FLAG_ACCESS_WRITE | TEE_DATA_FLAG_OVERWRITE,
                         TEE_HANDLE_NULL, NULL, 0u,
                         &obj);
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_storage: create object failed: 0x%x", res);
        return res;
    }

    res = TEE_WriteObjectData(obj, data_pv, data_len);
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_storage: write object data_pv failed: 0x%x", res);
        TEE_CloseAndDeletePersistentObject1(obj);
        return res;
    }
    TEE_CloseObject(obj);
    DMSG("tee_storage: wrote %zu bytes", data_len);
    return TEE_SUCCESS;
}

/* ── cmd_read_L ────────────────────────────────────────────────────────────── */

static TEE_Result cmd_read_L(uint32_t param_types_u32, TEE_Param params[4])
{
    const uint32_t exp_u32 = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                                         TEE_PARAM_TYPE_MEMREF_INOUT,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE);
    if (param_types_u32 != exp_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    const void *id_ptr_pv  = params[0].memref.buffer;
    uint32_t    id_len_u32  = (uint32_t)params[0].memref.size;

    if (!id_len_u32 || id_len_u32 > TS_MAX_ID_LEN)
        return TEE_ERROR_BAD_PARAMETERS;

    /* Copy objectID to TA-private stack buffer — see cmd_write_L comment. */
    uint8_t id_u8[TS_MAX_ID_LEN];
    TEE_MemMove(id_u8, id_ptr_pv, id_len_u32);

    TEE_ObjectHandle obj = TEE_HANDLE_NULL;
    TEE_Result res = open_for_read_L(id_u8, id_len_u32, &obj);
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_storage: open for read failed: 0x%x", res);
        return res;
    }

    uint32_t bytes_read_u32 = 0;
    res = TEE_ReadObjectData(obj,
                             params[1].memref.buffer,
                             params[1].memref.size,
                             &bytes_read_u32);
    TEE_CloseObject(obj);

    if (res == TEE_SUCCESS)
    {
        params[1].memref.size = bytes_read_u32;
        DMSG("tee_storage: read %u bytes", bytes_read_u32);
    } else
    {
        EMSG("tee_storage: read object data_pv failed: 0x%x", res);
    }

    return res;
}

/* ── cmd_erase_L ───────────────────────────────────────────────────────────── */

static TEE_Result cmd_erase_L(uint32_t param_types_u32, TEE_Param params[4])
{
    const uint32_t exp_u32 = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE);
    if (param_types_u32 != exp_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    const void *id_ptr_pv  = params[0].memref.buffer;
    uint32_t    id_len_u32  = (uint32_t)params[0].memref.size;

    if (!id_len_u32 || id_len_u32 > TS_MAX_ID_LEN)
        return TEE_ERROR_BAD_PARAMETERS;

    /* Copy objectID to TA-private stack buffer — see cmd_write_L comment. */
    uint8_t id_u8[TS_MAX_ID_LEN];
    TEE_MemMove(id_u8, id_ptr_pv, id_len_u32);

    TEE_ObjectHandle obj = TEE_HANDLE_NULL;
    TEE_Result res = open_for_delete_L(id_u8, id_len_u32, &obj);
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_storage: open for delete failed: 0x%x", res);
        return res;
    }

    res = TEE_CloseAndDeletePersistentObject1(obj);
    if (res != TEE_SUCCESS)
        EMSG("tee_storage: delete failed: 0x%x", res);
    else
        DMSG("tee_storage: object erased");

    return res;
}

/* ── cmd_list_L ────────────────────────────────────────────────────────────── */

static TEE_Result cmd_list_L(uint32_t param_types_u32, TEE_Param params[4])
{
    const uint32_t exp_u32 = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_OUTPUT,
                                         TEE_PARAM_TYPE_VALUE_OUTPUT,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE);
    if (param_types_u32 != exp_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    char    *out     = params[0].memref.buffer;
    size_t   out_cap = params[0].memref.size;
    size_t   out_used = 0;
    uint32_t count_u32   = 0;

    TEE_ObjectEnumHandle en = TEE_HANDLE_NULL;
    TEE_Result res = TEE_AllocatePersistentObjectEnumerator(&en);
    if (res != TEE_SUCCESS)
        return res;

    res = TEE_StartPersistentObjectEnumerator(en, TEE_STORAGE_PRIVATE);
    if (res == TEE_ERROR_ITEM_NOT_FOUND)
    {
        TEE_FreePersistentObjectEnumerator(en);
        params[0].memref.size = 0;
        params[1].value.a     = 0;
        return TEE_SUCCESS;
    }
    if (res != TEE_SUCCESS)
    {
        TEE_FreePersistentObjectEnumerator(en);
        return res;
    }

    uint8_t id_u8[TEE_OBJECT_ID_MAX_LEN];
    TEE_ObjectInfo info;

    while (true)
    {
        uint32_t id_len_u32 = sizeof(id_u8);
        res = TEE_GetNextPersistentObject(en, &info, id_u8, &id_len_u32);
        if (res == TEE_ERROR_ITEM_NOT_FOUND)
            break;
        if (res != TEE_SUCCESS)
            break;

        /* need id_len + 1 (NUL) to fit */
        if (out_used + id_len_u32 + 1 > out_cap)
        {
            res = TEE_ERROR_SHORT_BUFFER;
            break;
        }

        TEE_MemMove(out + out_used, id_u8, id_len_u32);
        out[out_used + id_len_u32] = '\0';
        out_used += id_len_u32 + 1u;
        count_u32++;
    }

    TEE_FreePersistentObjectEnumerator(en);

    params[0].memref.size = out_used;
    params[1].value.a     = count_u32;

    /* SHORT_BUFFER is expected when buffer is exhausted; treat as partial OK */
    return (res == TEE_ERROR_ITEM_NOT_FOUND || res == TEE_ERROR_SHORT_BUFFER)
           ? TEE_SUCCESS : res;
}

/* ── cmd_write_sector_L ────────────────────────────────────────────────────── */

static TEE_Result cmd_write_sector_L(uint32_t param_types_u32, TEE_Param params[4])
{
    const uint32_t exp_u32 = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                         TEE_PARAM_TYPE_MEMREF_INPUT,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE);
    if (param_types_u32 != exp_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    uint32_t    addr_u32     = params[0].value.a;
    const void *data_pv     = params[1].memref.buffer;
    size_t      data_len = params[1].memref.size;

    if (data_len > TS_MAX_DATA_LEN)
        return TEE_ERROR_EXCESS_DATA;

    char id_pv[10];
    make_sector_id_L(addr_u32, id_pv);

    TEE_ObjectHandle obj = TEE_HANDLE_NULL;
    TEE_Result res = TEE_CreatePersistentObject(
                         TEE_STORAGE_PRIVATE,
                         id_pv, 9u,
                         TEE_DATA_FLAG_ACCESS_WRITE | TEE_DATA_FLAG_OVERWRITE,
                         TEE_HANDLE_NULL, NULL, 0u,
                         &obj);
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_storage: create sector 0x%x failed: 0x%x", addr_u32, res);
        return res;
    }

    res = TEE_WriteObjectData(obj, data_pv, data_len);
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_storage: write sector 0x%x failed: 0x%x", addr_u32, res);
        TEE_CloseAndDeletePersistentObject1(obj);
        return res;
    }
    TEE_CloseObject(obj);
    DMSG("tee_storage: sector 0x%x: wrote %zu bytes", addr_u32, data_len);
    return TEE_SUCCESS;
}

/* ── cmd_read_sector_L ─────────────────────────────────────────────────────── */

static TEE_Result cmd_read_sector_L(uint32_t param_types_u32, TEE_Param params[4])
{
    const uint32_t exp_u32 = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                         TEE_PARAM_TYPE_MEMREF_INOUT,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE);
    if (param_types_u32 != exp_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    uint32_t addr_u32 = params[0].value.a;
    char id_pv[10];
    make_sector_id_L(addr_u32, id_pv);

    TEE_ObjectHandle obj = TEE_HANDLE_NULL;
    TEE_Result res = open_for_read_L(id_pv, 9u, &obj);
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_storage: open sector 0x%x failed: 0x%x", addr_u32, res);
        return res;
    }

    uint32_t bytes_read_u32 = 0;
    res = TEE_ReadObjectData(obj,
                             params[1].memref.buffer,
                             params[1].memref.size,
                             &bytes_read_u32);
    TEE_CloseObject(obj);

    if (res == TEE_SUCCESS)
    {
        params[1].memref.size = bytes_read_u32;
        DMSG("tee_storage: sector 0x%x: read %u bytes", addr_u32, bytes_read_u32);
    } else
    {
        EMSG("tee_storage: read sector 0x%x failed: 0x%x", addr_u32, res);
    }

    return res;
}

/* ── cmd_erase_sector_L ────────────────────────────────────────────────────── */

static TEE_Result cmd_erase_sector_L(uint32_t param_types_u32, TEE_Param params[4])
{
    const uint32_t exp_u32 = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE);
    if (param_types_u32 != exp_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    uint32_t addr_u32 = params[0].value.a;
    char id_pv[10];
    make_sector_id_L(addr_u32, id_pv);

    TEE_ObjectHandle obj = TEE_HANDLE_NULL;
    TEE_Result res = open_for_delete_L(id_pv, 9u, &obj);
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_storage: open sector 0x%x for delete: 0x%x", addr_u32, res);
        return res;
    }

    res = TEE_CloseAndDeletePersistentObject1(obj);
    if (res != TEE_SUCCESS)
        EMSG("tee_storage: erase sector 0x%x failed: 0x%x", addr_u32, res);
    else
        DMSG("tee_storage: sector 0x%x erased", addr_u32);

    return res;
}

/* ── cmd_format_L ──────────────────────────────────────────────────────────── */
/*
 * Delete every persistent object, one at a time.
 * Approach: restart enumerator each iteration, get the first object, stop
 * enumerator, delete that object, repeat.  O(n²) but safe and avoids
 * needing to heap-allocate a list of all IDs.
 */
static TEE_Result cmd_format_L(void)
{
    uint8_t  id_u8[TEE_OBJECT_ID_MAX_LEN];
    uint32_t deleted_u32 = 0;

    while (true)
    {
        TEE_ObjectEnumHandle en = TEE_HANDLE_NULL;
        TEE_Result res = TEE_AllocatePersistentObjectEnumerator(&en);
        if (res != TEE_SUCCESS)
            return res;

        res = TEE_StartPersistentObjectEnumerator(en, TEE_STORAGE_PRIVATE);
        if (res == TEE_ERROR_ITEM_NOT_FOUND)
        {
            TEE_FreePersistentObjectEnumerator(en);
            DMSG("tee_storage: format complete, %u object(s) deleted_u32", deleted_u32);
            return TEE_SUCCESS;
        }
        if (res != TEE_SUCCESS)
        {
            TEE_FreePersistentObjectEnumerator(en);
            return res;
        }

        TEE_ObjectInfo info;
        uint32_t id_len_u32 = sizeof(id_u8);
        res = TEE_GetNextPersistentObject(en, &info, id_u8, &id_len_u32);
        TEE_FreePersistentObjectEnumerator(en);

        if (res == TEE_ERROR_ITEM_NOT_FOUND)
            return TEE_SUCCESS;
        if (res != TEE_SUCCESS)
            return res;

        TEE_ObjectHandle obj = TEE_HANDLE_NULL;
        res = open_for_delete_L(id_u8, id_len_u32, &obj);
        if (res == TEE_SUCCESS)
        {
            res = TEE_CloseAndDeletePersistentObject1(obj);
            if (res == TEE_SUCCESS)
                deleted_u32++;
            else
            {
                EMSG("tee_storage: format: delete failed 0x%x", res);
                return res;
            }
        } else
        {
            EMSG("tee_storage: format: open for delete failed 0x%x", res);
            /* Don't loop forever on persistent error */
            return res;
        }
    }
}

/* ── cmd_info_L ────────────────────────────────────────────────────────────── */

static TEE_Result cmd_info_L(uint32_t param_types_u32, TEE_Param params[4])
{
    const uint32_t exp_u32 = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
                                         TEE_PARAM_TYPE_VALUE_OUTPUT,
                                         TEE_PARAM_TYPE_NONE,
                                         TEE_PARAM_TYPE_NONE);
    if (param_types_u32 != exp_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    TEE_ObjectEnumHandle en = TEE_HANDLE_NULL;
    TEE_Result res = TEE_AllocatePersistentObjectEnumerator(&en);
    if (res != TEE_SUCCESS)
        return res;

    res = TEE_StartPersistentObjectEnumerator(en, TEE_STORAGE_PRIVATE);
    if (res == TEE_ERROR_ITEM_NOT_FOUND)
    {
        TEE_FreePersistentObjectEnumerator(en);
        params[0].value.a = 0;
        params[1].value.a = 0;
        return TEE_SUCCESS;
    }
    if (res != TEE_SUCCESS)
    {
        TEE_FreePersistentObjectEnumerator(en);
        return res;
    }

    uint32_t count_u32 = 0;
    uint32_t total_bytes_u32 = 0;
    uint8_t  id_u8[TEE_OBJECT_ID_MAX_LEN];
    TEE_ObjectInfo info;

    while (true)
    {
        uint32_t id_len_u32 = sizeof(id_u8);
        res = TEE_GetNextPersistentObject(en, &info, id_u8, &id_len_u32);
        if (res == TEE_ERROR_ITEM_NOT_FOUND)
            break;
        if (res != TEE_SUCCESS)
            break;
        count_u32++;
        total_bytes_u32 += info.dataSize;
    }

    TEE_FreePersistentObjectEnumerator(en);

    params[0].value.a = count_u32;
    params[1].value.a = total_bytes_u32;

    return (res == TEE_ERROR_ITEM_NOT_FOUND) ? TEE_SUCCESS : res;
}

/* ── Command dispatcher ──────────────────────────────────────────────────── */

TEE_Result TA_InvokeCommandEntryPoint(void *sess_ctx_pv __unused,
                                      uint32_t cmd_id_u32,
                                      uint32_t param_types_u32,
                                      TEE_Param params[4])
{
    switch (cmd_id_u32)
    {
    case CMD_WRITE:         return cmd_write_L(param_types_u32, params);
    case CMD_READ:          return cmd_read_L(param_types_u32, params);
    case CMD_ERASE:         return cmd_erase_L(param_types_u32, params);
    case CMD_LIST:          return cmd_list_L(param_types_u32, params);
    case CMD_WRITE_SECTOR:  return cmd_write_sector_L(param_types_u32, params);
    case CMD_READ_SECTOR:   return cmd_read_sector_L(param_types_u32, params);
    case CMD_ERASE_SECTOR:  return cmd_erase_sector_L(param_types_u32, params);
    case CMD_FORMAT:        return cmd_format_L();
    case CMD_INFO:          return cmd_info_L(param_types_u32, params);
    default:
        EMSG("tee_storage: unknown command 0x%x", cmd_id_u32);
        return TEE_ERROR_NOT_SUPPORTED;
    }
}
