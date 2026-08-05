/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       tee_lfs_storage.c
* @brief      LittleFS-based secure storage backend for OP-TEE on W77Q flash
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/

// SPDX-License-Identifier: BSD-2-Clause
/*
 * Replaces OP-TEE's default REE_FS / RPMB_FS backends when CFG_W77Q_FS=y.
 *
 * Object files are stored under /<ta_uuid_hex>/<obj_id_hex> in a LittleFS
 * filesystem.  All flash I/O is handled by the w77q_lfs_port layer; this
 * file only calls lfs_* and W77Q_LFS_PORT_* APIs.
 */

/*-----------------------------------------------------------------------------------------------------------
                                                 INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include "tee_lfs_storage.h"
#include "w77q_lfs_port.h"

#include <initcall.h>
#include <kernel/mutex.h>
#include <kernel/user_access.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <tee/tee_fs.h>
#include <tee/tee_pobj.h>
#include <trace.h>
#include <util.h>

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/
/* Hex chars per byte */
#define HEX_PER_BYTE 2U

/* UUID size in bytes */
#define UUID_SIZE 16U

/*-----------------------------------------------------------------------------------------------------------
                                              LOCAL VARIABLES
-----------------------------------------------------------------------------------------------------------*/
static struct mutex g_lock_L = MUTEX_INITIALIZER;
static bool g_ready_b_L;

static lfs_t g_lfs_L;
static struct lfs_config g_lfsCfg_L;

/*-----------------------------------------------------------------------------------------------------------
                                          LOCAL FUNCTION PROTOTYPES
-----------------------------------------------------------------------------------------------------------*/
static void uuid_to_hex_L(const TEE_UUID *uuid_ps, char *out_pu8);
static void bytes_to_hex_L(const uint8_t *src_pu8, size_t len_u32, char *out_pu8);
static size_t hex_to_bytes_L(const char *hex_pu8, uint8_t *out_pu8, size_t max_u32);
static void build_path_L(const struct tee_pobj *po_ps, char *path_pu8);
static void build_dir_path_L(const TEE_UUID *uuid_ps, char *dirPath_pu8);
static TEE_Result ensure_ta_dir_L(const TEE_UUID *uuid_ps);
static TEE_Result zeroize_file_L(const char *path_pu8);
static int mark_block_cb_L(void *ctx_pv, lfs_block_t block_u32);
static TEE_Result lfs_to_tee_err_L(int lfsErr_i32);

static TEE_Result tee_lfs_open_L(struct tee_pobj *po, size_t *size_u32,
                                 struct tee_file_handle **fh);
static TEE_Result tee_lfs_create_L(struct tee_pobj *po, bool overwrite_b,
                                   const void *head_pv, size_t head_size,
                                   const void *attr_pv, size_t attr_size,
                                   const void *data_core_pv, const void *data_user_pv,
                                   size_t data_size_u32, struct tee_file_handle **fh);
static void tee_lfs_close_L(struct tee_file_handle **fh);
static TEE_Result tee_lfs_read_L(struct tee_file_handle *fh, size_t pos,
                                 void *buf_core_pv, void *buf_user_pv, size_t *len_u8);
static TEE_Result tee_lfs_write_L(struct tee_file_handle *fh, size_t pos,
                                  const void *buf_core_pv, const void *buf_user_pv,
                                  size_t len_u8);
static TEE_Result tee_lfs_truncate_L(struct tee_file_handle *fh, size_t size_u32);
static TEE_Result tee_lfs_remove_L(struct tee_pobj *po);
static TEE_Result tee_lfs_rename_L(struct tee_pobj *old_po, struct tee_pobj *new_po,
                                   bool overwrite_b);
static TEE_Result tee_lfs_opendir_L(const TEE_UUID *uuid,
                                    struct tee_fs_dir **out_dir);
static TEE_Result tee_lfs_readdir_L(struct tee_fs_dir *d_ps,
                                    struct tee_fs_dirent **out_ent);
static void tee_lfs_closedir_L(struct tee_fs_dir *d_ps);

/*-----------------------------------------------------------------------------------------------------------
                                            INTERFACE FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

TEE_Result tee_lfs_init(void)
{
    TEE_Result res = TEE_SUCCESS;
    int lfsRes_i32 = 0;

    IMSG("tee_lfs_storage: >>> init enter");

    /* ---- Hardware init (delegated to port layer) ---- */
    res = W77Q_LFS_PORT_HwInit();
    if (res != TEE_SUCCESS)
        return res;

    /* ---- LittleFS config ---- */
    W77Q_LFS_PORT_Init(&g_lfsCfg_L);

    /* ---- Mount (or format on first boot) ---- */
    lfsRes_i32 = lfs_mount(&g_lfs_L, &g_lfsCfg_L);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        IMSG("tee_lfs_storage: mount failed (%d) - formatting", lfsRes_i32);
        lfsRes_i32 = lfs_format(&g_lfs_L, &g_lfsCfg_L);
        if (lfsRes_i32 != LFS_ERR_OK)
        {
            EMSG("tee_lfs_storage: format failed: %d", lfsRes_i32);
            return TEE_ERROR_GENERIC;
        }
        lfsRes_i32 = lfs_mount(&g_lfs_L, &g_lfsCfg_L);
        if (lfsRes_i32 != LFS_ERR_OK)
        {
            EMSG("tee_lfs_storage: mount after format failed: %d", lfsRes_i32);
            return TEE_ERROR_GENERIC;
        }
    }

    g_ready_b_L = true;
    IMSG("tee_lfs_storage: ready (LittleFS mounted)");
    return TEE_SUCCESS;
}

driver_init(tee_lfs_init);

bool tee_lfs_is_ready(void)
{
    return g_ready_b_L;
}

/* ---------------------------------------------------------------------------
 * tee_file_operations export
 * --------------------------------------------------------------------------*/

const struct tee_file_operations tee_lfs_ops =
{
    .open     = tee_lfs_open_L,
    .create   = tee_lfs_create_L,
    .close    = tee_lfs_close_L,
    .read     = tee_lfs_read_L,
    .write    = tee_lfs_write_L,
    .truncate = tee_lfs_truncate_L,
    .remove   = tee_lfs_remove_L,
    .rename   = tee_lfs_rename_L,
    .opendir  = tee_lfs_opendir_L,
    .readdir  = tee_lfs_readdir_L,
    .closedir = tee_lfs_closedir_L,
};

/* ---------------------------------------------------------------------------
 * Diagnostic / raw-access APIs (delegate to port layer)
 * --------------------------------------------------------------------------*/

void tee_lfs_list_all(tee_lfs_list_all_cb cb, void *ctx_pv)
{
    lfs_dir_t rootDir_s;
    struct lfs_info rootInfo_s;

    mutex_lock(&g_lock_L);

    if (lfs_dir_open(&g_lfs_L, &rootDir_s, "/") != LFS_ERR_OK)
    {
        mutex_unlock(&g_lock_L);
        return;
    }

    /* Iterate TA directories */
    while (lfs_dir_read(&g_lfs_L, &rootDir_s, &rootInfo_s) > 0)
    {
        lfs_dir_t taDir_s;
        struct lfs_info fileInfo_s;
        char taPath_au8[34];
        uint8_t taUuid_au8[UUID_SIZE];

        if (rootInfo_s.type != LFS_TYPE_DIR)
            continue;
        if (rootInfo_s.name[0] == '.')
            continue;

        /* Decode UUID from directory name */
        if (strlen(rootInfo_s.name) != UUID_SIZE * HEX_PER_BYTE)
            continue;
        hex_to_bytes_L(rootInfo_s.name, taUuid_au8, UUID_SIZE);

        snprintf(taPath_au8, sizeof(taPath_au8), "/%s", rootInfo_s.name);

        if (lfs_dir_open(&g_lfs_L, &taDir_s, taPath_au8) != LFS_ERR_OK)
            continue;

        /* Iterate files within TA directory */
        while (lfs_dir_read(&g_lfs_L, &taDir_s, &fileInfo_s) > 0)
        {
            uint8_t objId_au8[TEE_OBJECT_ID_MAX_LEN];
            size_t objIdLen_u32 = 0;

            if (fileInfo_s.type != LFS_TYPE_REG)
                continue;

            objIdLen_u32 = hex_to_bytes_L(fileInfo_s.name, objId_au8,
                                          sizeof(objId_au8));
            if (objIdLen_u32 == 0)
                continue;

            /* Pass the raw decoded obj_id bytes together with their length so
             * that binary or NUL-containing object IDs survive intact. */
            if (cb(taUuid_au8, 0, fileInfo_s.size,
                   objId_au8, (uint32_t)objIdLen_u32, ctx_pv) != 0)
            {
                lfs_dir_close(&g_lfs_L, &taDir_s);
                goto done;
            }
        }
        lfs_dir_close(&g_lfs_L, &taDir_s);
    }

done:
    lfs_dir_close(&g_lfs_L, &rootDir_s);
    mutex_unlock(&g_lock_L);
}

TEE_Result tee_lfs_read_raw(uint32_t off_u32, void *buf_pv, size_t len_u32)
{
    TEE_Result res;

    mutex_lock(&g_lock_L);
    res = W77Q_LFS_PORT_ReadRaw(off_u32, buf_pv, len_u32);
    mutex_unlock(&g_lock_L);
    return res;
}

TEE_Result tee_lfs_read_plain(uint32_t off_u32, void *buf_pv, size_t len_u32)
{
    TEE_Result res;

    mutex_lock(&g_lock_L);
    res = W77Q_LFS_PORT_ReadPlain(off_u32, buf_pv, len_u32);
    mutex_unlock(&g_lock_L);
    return res;
}

TEE_Result tee_lfs_read_file(const uint8_t ta_uuid_u8[16],
                             const char *obj_id_pu8, size_t obj_id_len_u32,
                             void *buf_pv, size_t buf_size_u32,
                             size_t *out_size_pu32)
{
    char path_au8[TEE_LFS_PATH_MAX];
    char uuidHex_au8[(UUID_SIZE * HEX_PER_BYTE) + 1U];
    char objIdHex_au8[(TEE_OBJECT_ID_MAX_LEN * HEX_PER_BYTE) + 1U];
    lfs_file_t file_s;
    struct lfs_file_config fileCfg_s;
    uint8_t fileBuf_au8[W77Q_LFS_CACHE_SIZE];
    int lfsRes_i32 = 0;
    lfs_ssize_t readRes_i32 = 0;
    struct lfs_info info_s;

    bytes_to_hex_L(ta_uuid_u8, UUID_SIZE, uuidHex_au8);
    bytes_to_hex_L((const uint8_t *)obj_id_pu8, obj_id_len_u32, objIdHex_au8);
    snprintf(path_au8, sizeof(path_au8), "/%s/%s", uuidHex_au8, objIdHex_au8);

    mutex_lock(&g_lock_L);

    lfsRes_i32 = lfs_stat(&g_lfs_L, path_au8, &info_s);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        mutex_unlock(&g_lock_L);
        return lfs_to_tee_err_L(lfsRes_i32);
    }

    memset(&fileCfg_s, 0, sizeof(fileCfg_s));
    fileCfg_s.buffer = fileBuf_au8;

    lfsRes_i32 = lfs_file_opencfg(&g_lfs_L, &file_s, path_au8,
                                  LFS_O_RDONLY, &fileCfg_s);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        mutex_unlock(&g_lock_L);
        return lfs_to_tee_err_L(lfsRes_i32);
    }

    {
        size_t toRead_u32 = (info_s.size < buf_size_u32) ?
                             info_s.size : buf_size_u32;

        readRes_i32 = lfs_file_read(&g_lfs_L, &file_s, buf_pv, toRead_u32);
    }

    lfs_file_close(&g_lfs_L, &file_s);
    mutex_unlock(&g_lock_L);

    if (readRes_i32 < 0)
        return TEE_ERROR_GENERIC;

    if (out_size_pu32 != NULL)
        *out_size_pu32 = (size_t)readRes_i32;
    return TEE_SUCCESS;
}

TEE_Result tee_lfs_write_raw(uint32_t off_u32, const void *buf_pv, size_t len_u32)
{
    TEE_Result res;

    mutex_lock(&g_lock_L);
    res = W77Q_LFS_PORT_WriteRaw(off_u32, buf_pv, len_u32);
    mutex_unlock(&g_lock_L);
    return res;
}

TEE_Result tee_lfs_erase_sector(uint32_t off_u32)
{
    TEE_Result res;
    int lfsRes_i32 = 0;

    mutex_lock(&g_lock_L);
    IMSG("tee_lfs_storage: erasing sector at 0x%08" PRIx32, off_u32);
    res = W77Q_LFS_PORT_EraseSector(off_u32);
    if (res == TEE_SUCCESS)
    {
        /* Remount — erased sector may have corrupted LittleFS metadata */
        lfs_unmount(&g_lfs_L);
        lfsRes_i32 = lfs_mount(&g_lfs_L, &g_lfsCfg_L);
        if (lfsRes_i32 != LFS_ERR_OK)
        {
            /* Superblock likely destroyed — reformat */
            IMSG("tee_lfs_storage: remount failed (%d) - reformatting", lfsRes_i32);
            lfsRes_i32 = lfs_format(&g_lfs_L, &g_lfsCfg_L);
            if (lfsRes_i32 == LFS_ERR_OK)
                lfsRes_i32 = lfs_mount(&g_lfs_L, &g_lfsCfg_L);
            if (lfsRes_i32 != LFS_ERR_OK)
            {
                EMSG("tee_lfs_storage: format+mount after erase failed: %d", lfsRes_i32);
                res = TEE_ERROR_GENERIC;
            }
        }
    }
    mutex_unlock(&g_lock_L);
    return res;
}

TEE_Result tee_lfs_erase_chip(void)
{
    TEE_Result res = TEE_SUCCESS;
    int lfsRes_i32 = 0;

    mutex_lock(&g_lock_L);
    IMSG("tee_lfs_storage: erasing entire flash partition");

    lfs_unmount(&g_lfs_L);

    res = W77Q_LFS_PORT_ErasePartition();
    if (res != TEE_SUCCESS)
    {
        EMSG("tee_lfs_storage: partition erase failed: %#" PRIx32, res);
        goto out;
    }

    IMSG("tee_lfs_storage: erase complete - reformatting");

    lfsRes_i32 = lfs_format(&g_lfs_L, &g_lfsCfg_L);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        EMSG("tee_lfs_storage: format after erase failed: %d", lfsRes_i32);
        res = TEE_ERROR_GENERIC;
        goto out;
    }

    lfsRes_i32 = lfs_mount(&g_lfs_L, &g_lfsCfg_L);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        EMSG("tee_lfs_storage: mount after format failed: %d", lfsRes_i32);
        res = TEE_ERROR_GENERIC;
        goto out;
    }

    IMSG("tee_lfs_storage: storage reset complete - 0 objects, ready");

out:
    mutex_unlock(&g_lock_L);
    return res;
}

/*-----------------------------------------------------------------------------------------------------------
                                              LOCAL FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Hex encoding / decoding helpers
 * --------------------------------------------------------------------------*/

static const char g_hexChars_L[] = "0123456789abcdef";

static void uuid_to_hex_L(const TEE_UUID *uuid_ps, char *out_pu8)
{
    bytes_to_hex_L((const uint8_t *)uuid_ps, sizeof(TEE_UUID), out_pu8);
}

static void bytes_to_hex_L(const uint8_t *src_pu8, size_t len_u32, char *out_pu8)
{
    size_t i = 0;

    for (i = 0; i < len_u32; i++)
    {
        out_pu8[i * HEX_PER_BYTE]       = g_hexChars_L[(src_pu8[i] >> 4U) & 0x0FU];
        out_pu8[(i * HEX_PER_BYTE) + 1U] = g_hexChars_L[src_pu8[i] & 0x0FU];
    }
    out_pu8[len_u32 * HEX_PER_BYTE] = '\0';
}

static uint8_t hex_nibble_L(char c_u8)
{
    if (c_u8 >= '0' && c_u8 <= '9')
        return (uint8_t)(c_u8 - '0');
    if (c_u8 >= 'a' && c_u8 <= 'f')
        return (uint8_t)(c_u8 - 'a' + 10);
    if (c_u8 >= 'A' && c_u8 <= 'F')
        return (uint8_t)(c_u8 - 'A' + 10);
    return 0xFFU;
}

static size_t hex_to_bytes_L(const char *hex_pu8, uint8_t *out_pu8, size_t max_u32)
{
    size_t hexLen_u32 = strlen(hex_pu8);
    size_t byteLen_u32 = hexLen_u32 / HEX_PER_BYTE;
    size_t i = 0;

    if ((hexLen_u32 & 1U) != 0 || byteLen_u32 > max_u32)
        return 0;

    for (i = 0; i < byteLen_u32; i++)
    {
        uint8_t hi_u8 = hex_nibble_L(hex_pu8[i * HEX_PER_BYTE]);
        uint8_t lo_u8 = hex_nibble_L(hex_pu8[(i * HEX_PER_BYTE) + 1U]);

        if (hi_u8 > 0x0FU || lo_u8 > 0x0FU)
            return 0;
        out_pu8[i] = (uint8_t)((hi_u8 << 4U) | lo_u8);
    }
    return byteLen_u32;
}

/* ---------------------------------------------------------------------------
 * Path helpers
 * --------------------------------------------------------------------------*/

static void build_dir_path_L(const TEE_UUID *uuid_ps, char *dirPath_pu8)
{
    char hexBuf_au8[(UUID_SIZE * HEX_PER_BYTE) + 1U];

    uuid_to_hex_L(uuid_ps, hexBuf_au8);
    snprintf(dirPath_pu8, 34, "/%s", hexBuf_au8);
}

static void build_path_L(const struct tee_pobj *po_ps, char *path_pu8)
{
    char uuidHex_au8[(UUID_SIZE * HEX_PER_BYTE) + 1U];
    char objIdHex_au8[(TEE_OBJECT_ID_MAX_LEN * HEX_PER_BYTE) + 1U];

    uuid_to_hex_L(&po_ps->uuid, uuidHex_au8);
    bytes_to_hex_L(po_ps->obj_id, po_ps->obj_id_len, objIdHex_au8);
    snprintf(path_pu8, TEE_LFS_PATH_MAX, "/%s/%s", uuidHex_au8, objIdHex_au8);
}

static TEE_Result ensure_ta_dir_L(const TEE_UUID *uuid_ps)
{
    char dirPath_au8[34];
    int lfsRes_i32 = 0;

    build_dir_path_L(uuid_ps, dirPath_au8);
    lfsRes_i32 = lfs_mkdir(&g_lfs_L, dirPath_au8);
    if (lfsRes_i32 != LFS_ERR_OK && lfsRes_i32 != LFS_ERR_EXIST)
        return lfs_to_tee_err_L(lfsRes_i32);
    return TEE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Error mapping
 * --------------------------------------------------------------------------*/

static TEE_Result lfs_to_tee_err_L(int lfsErr_i32)
{
    switch (lfsErr_i32)
    {
        case LFS_ERR_OK:
            return TEE_SUCCESS;
        case LFS_ERR_NOENT:
            return TEE_ERROR_ITEM_NOT_FOUND;
        case LFS_ERR_EXIST:
            return TEE_ERROR_ACCESS_CONFLICT;
        case LFS_ERR_NOSPC:
            return TEE_ERROR_STORAGE_NO_SPACE;
        case LFS_ERR_NOMEM:
            return TEE_ERROR_OUT_OF_MEMORY;
        case LFS_ERR_CORRUPT:
            return TEE_ERROR_CORRUPT_OBJECT;
        case LFS_ERR_INVAL:
            return TEE_ERROR_BAD_PARAMETERS;
        default:
            return TEE_ERROR_GENERIC;
    }
}

/* ---------------------------------------------------------------------------
 * Block traversal callback for secure deletion
 * --------------------------------------------------------------------------*/

static int mark_block_cb_L(void *ctx_pv, lfs_block_t block_u32)
{
    uint8_t *bitmap_pu8 = (uint8_t *)ctx_pv;

    if (block_u32 < W77Q_LFS_BLOCK_COUNT)
        bitmap_pu8[block_u32 / 8U] |= (uint8_t)(1U << (block_u32 % 8U));
    return 0;
}

/* ---------------------------------------------------------------------------
 * Secure zeroize
 * --------------------------------------------------------------------------*/

static TEE_Result zeroize_file_L(const char *path_pu8)
{
    lfs_file_t file_s;
    struct lfs_file_config fileCfg_s;
    uint8_t fileBuf_au8[W77Q_LFS_CACHE_SIZE];
    uint8_t zeros_au8[256];
    struct lfs_info info_s;
    int lfsRes_i32 = 0;
    lfs_ssize_t written_i32 = 0;
    lfs_size_t remaining_u32 = 0;

    lfsRes_i32 = lfs_stat(&g_lfs_L, path_pu8, &info_s);
    if (lfsRes_i32 != LFS_ERR_OK || info_s.size == 0)
        return TEE_SUCCESS;

    memset(&fileCfg_s, 0, sizeof(fileCfg_s));
    fileCfg_s.buffer = fileBuf_au8;

    lfsRes_i32 = lfs_file_opencfg(&g_lfs_L, &file_s, path_pu8,
                                  LFS_O_WRONLY, &fileCfg_s);
    if (lfsRes_i32 != LFS_ERR_OK)
        return lfs_to_tee_err_L(lfsRes_i32);

    memset(zeros_au8, 0, sizeof(zeros_au8));
    remaining_u32 = info_s.size;

    while (remaining_u32 > 0)
    {
        lfs_size_t chunk_u32 = (remaining_u32 > sizeof(zeros_au8))
                                ? (lfs_size_t)sizeof(zeros_au8) : remaining_u32;

        written_i32 = lfs_file_write(&g_lfs_L, &file_s, zeros_au8, chunk_u32);
        if (written_i32 < 0)
        {
            lfs_file_close(&g_lfs_L, &file_s);
            return TEE_ERROR_GENERIC;
        }
        remaining_u32 -= (lfs_size_t)written_i32;
    }

    lfs_file_close(&g_lfs_L, &file_s);
    return TEE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * tee_file_operations callbacks
 * --------------------------------------------------------------------------*/

static TEE_Result tee_lfs_open_L(struct tee_pobj *po, size_t *size_u32,
                                 struct tee_file_handle **fh)
{
    struct tee_lfs_fh *h_ps = NULL;
    int lfsRes_i32 = 0;

    h_ps = calloc(1u, sizeof(*h_ps));
    if (h_ps == NULL)
        return TEE_ERROR_OUT_OF_MEMORY;

    build_path_L(po, h_ps->path_au8);

    memset(&h_ps->fileCfg_s, 0, sizeof(h_ps->fileCfg_s));
    h_ps->fileCfg_s.buffer = h_ps->fileBuf_au8;

    mutex_lock(&g_lock_L);

    lfsRes_i32 = lfs_file_opencfg(&g_lfs_L, &h_ps->file_s, h_ps->path_au8,
                                  LFS_O_RDWR, &h_ps->fileCfg_s);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        mutex_unlock(&g_lock_L);
        free(h_ps);
        return lfs_to_tee_err_L(lfsRes_i32);
    }

    h_ps->dataSize_u32 = (uint32_t)lfs_file_size(&g_lfs_L, &h_ps->file_s);

    if (size_u32 != NULL)
        *size_u32 = h_ps->dataSize_u32;

    *fh = (struct tee_file_handle *)h_ps;
    mutex_unlock(&g_lock_L);
    return TEE_SUCCESS;
}

static TEE_Result tee_lfs_create_L(struct tee_pobj *po, bool overwrite_b,
                                   const void *head_pv, size_t head_size,
                                   const void *attr_pv, size_t attr_size,
                                   const void *data_core_pv, const void *data_user_pv,
                                   size_t data_size_u32, struct tee_file_handle **fh)
{
    struct tee_lfs_fh *h_ps = NULL;
    uint32_t totalPayload_u32 = 0;
    uint8_t *payload_pu8 = NULL;
    int lfsRes_i32 = 0;
    int flags_i32 = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
    TEE_Result res = TEE_SUCCESS;
    lfs_ssize_t written_i32 = 0;
    char path_au8[TEE_LFS_PATH_MAX];

    build_path_L(po, path_au8);

    /* Compute total payload = head | attr | data */
    if (__builtin_add_overflow(head_size, attr_size, &totalPayload_u32) ||
        __builtin_add_overflow(totalPayload_u32, data_size_u32, &totalPayload_u32))
        return TEE_ERROR_EXCESS_DATA;

    /* Assemble payload into a contiguous buffer */
    if (totalPayload_u32 > 0)
    {
        payload_pu8 = calloc(1u, totalPayload_u32);
        if (payload_pu8 == NULL)
            return TEE_ERROR_OUT_OF_MEMORY;

        if (head_pv != NULL && head_size > 0)
            memcpy(payload_pu8, head_pv, head_size);

        if (attr_pv != NULL && attr_size > 0)
            memcpy(payload_pu8 + head_size, attr_pv, attr_size);

        if (data_size_u32 > 0)
        {
            if (data_core_pv != NULL)
            {
                memcpy(payload_pu8 + head_size + attr_size, data_core_pv,
                       data_size_u32);
            }
            else if (data_user_pv != NULL)
            {
                res = copy_from_user(payload_pu8 + head_size + attr_size,
                                     data_user_pv, data_size_u32);
                if (res != TEE_SUCCESS)
                    goto out;
            }
        }
    }

    mutex_lock(&g_lock_L);

    /* Check existence when overwrite is not allowed */
    if (!overwrite_b)
    {
        struct lfs_info info_s;

        if (lfs_stat(&g_lfs_L, path_au8, &info_s) == LFS_ERR_OK)
        {
            mutex_unlock(&g_lock_L);
            res = TEE_ERROR_ACCESS_CONFLICT;
            goto out;
        }
    }
    else
    {
        /* Zeroize old file content before overwrite */
        zeroize_file_L(path_au8);
    }

    /* Ensure TA directory exists */
    res = ensure_ta_dir_L(&po->uuid);
    if (res != TEE_SUCCESS)
    {
        mutex_unlock(&g_lock_L);
        goto out;
    }

    h_ps = calloc(1u, sizeof(*h_ps));
    if (h_ps == NULL)
    {
        mutex_unlock(&g_lock_L);
        res = TEE_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    memcpy(h_ps->path_au8, path_au8, TEE_LFS_PATH_MAX);
    memset(&h_ps->fileCfg_s, 0, sizeof(h_ps->fileCfg_s));
    h_ps->fileCfg_s.buffer = h_ps->fileBuf_au8;

    lfsRes_i32 = lfs_file_opencfg(&g_lfs_L, &h_ps->file_s, h_ps->path_au8,
                                  flags_i32, &h_ps->fileCfg_s);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        mutex_unlock(&g_lock_L);
        free(h_ps);
        h_ps = NULL;
        res = lfs_to_tee_err_L(lfsRes_i32);
        goto out;
    }

    /* Write payload */
    if (payload_pu8 != NULL && totalPayload_u32 > 0)
    {
        written_i32 = lfs_file_write(&g_lfs_L, &h_ps->file_s,
                                     payload_pu8, totalPayload_u32);
        if (written_i32 < 0 || (lfs_size_t)written_i32 != totalPayload_u32)
        {
            lfs_file_close(&g_lfs_L, &h_ps->file_s);
            lfs_remove(&g_lfs_L, h_ps->path_au8);
            mutex_unlock(&g_lock_L);
            free(h_ps);
            h_ps = NULL;
            res = TEE_ERROR_GENERIC;
            goto out;
        }
    }

    h_ps->dataSize_u32 = totalPayload_u32;

    if (fh != NULL)
    {
        /* Re-open as RDWR for subsequent read/write calls */
        lfs_file_close(&g_lfs_L, &h_ps->file_s);
        lfsRes_i32 = lfs_file_opencfg(&g_lfs_L, &h_ps->file_s, h_ps->path_au8,
                                      LFS_O_RDWR, &h_ps->fileCfg_s);
        if (lfsRes_i32 != LFS_ERR_OK)
        {
            mutex_unlock(&g_lock_L);
            free(h_ps);
            h_ps = NULL;
            res = lfs_to_tee_err_L(lfsRes_i32);
            goto out;
        }
        *fh = (struct tee_file_handle *)h_ps;
    }
    else
    {
        /* Caller doesn't need the handle — close immediately */
        lfs_file_close(&g_lfs_L, &h_ps->file_s);
        free(h_ps);
        h_ps = NULL;
    }

    mutex_unlock(&g_lock_L);

out:
    free(payload_pu8);
    return res;
}

static void tee_lfs_close_L(struct tee_file_handle **fh)
{
    if (fh != NULL && *fh != NULL)
    {
        struct tee_lfs_fh *h_ps = (struct tee_lfs_fh *)*fh;

        mutex_lock(&g_lock_L);
        lfs_file_close(&g_lfs_L, &h_ps->file_s);
        mutex_unlock(&g_lock_L);
        free(h_ps);
        *fh = NULL;
    }
}

static TEE_Result tee_lfs_read_L(struct tee_file_handle *fh, size_t pos,
                                 void *buf_core_pv, void *buf_user_pv,
                                 size_t *len_u8)
{
    struct tee_lfs_fh *h_ps = (struct tee_lfs_fh *)fh;
    size_t toRead_u32 = 0;
    lfs_ssize_t readRes_i32 = 0;
    TEE_Result res = TEE_SUCCESS;

    mutex_lock(&g_lock_L);

    if (pos >= h_ps->dataSize_u32)
    {
        *len_u8 = 0;
        mutex_unlock(&g_lock_L);
        return TEE_SUCCESS;
    }

    toRead_u32 = MIN(*len_u8, h_ps->dataSize_u32 - pos);

    lfs_file_seek(&g_lfs_L, &h_ps->file_s, (lfs_soff_t)pos, LFS_SEEK_SET);

    if (buf_core_pv != NULL)
    {
        readRes_i32 = lfs_file_read(&g_lfs_L, &h_ps->file_s,
                                    buf_core_pv, toRead_u32);
        if (readRes_i32 < 0)
        {
            mutex_unlock(&g_lock_L);
            return TEE_ERROR_GENERIC;
        }
        *len_u8 = (size_t)readRes_i32;
    }
    else if (buf_user_pv != NULL)
    {
        void *tmp_pv = malloc(toRead_u32);

        if (tmp_pv == NULL)
        {
            mutex_unlock(&g_lock_L);
            return TEE_ERROR_OUT_OF_MEMORY;
        }
        readRes_i32 = lfs_file_read(&g_lfs_L, &h_ps->file_s, tmp_pv, toRead_u32);
        if (readRes_i32 < 0)
        {
            free(tmp_pv);
            mutex_unlock(&g_lock_L);
            return TEE_ERROR_GENERIC;
        }
        res = copy_to_user(buf_user_pv, tmp_pv, (size_t)readRes_i32);
        free(tmp_pv);
        if (res == TEE_SUCCESS)
            *len_u8 = (size_t)readRes_i32;
    }

    mutex_unlock(&g_lock_L);
    return res;
}

static TEE_Result tee_lfs_write_L(struct tee_file_handle *fh, size_t pos,
                                  const void *buf_core_pv, const void *buf_user_pv,
                                  size_t len_u8)
{
    struct tee_lfs_fh *h_ps = (struct tee_lfs_fh *)fh;
    lfs_ssize_t written_i32 = 0;
    TEE_Result res = TEE_SUCCESS;
    uint8_t *srcBuf_pu8 = NULL;
    bool needFree_b = false;

    if (len_u8 == 0)
        return TEE_SUCCESS;

    /* Resolve source buffer */
    if (buf_core_pv != NULL)
    {
        srcBuf_pu8 = (uint8_t *)(uintptr_t)buf_core_pv;
    }
    else if (buf_user_pv != NULL)
    {
        srcBuf_pu8 = malloc(len_u8);
        if (srcBuf_pu8 == NULL)
            return TEE_ERROR_OUT_OF_MEMORY;
        res = copy_from_user(srcBuf_pu8, buf_user_pv, len_u8);
        if (res != TEE_SUCCESS)
        {
            free(srcBuf_pu8);
            return res;
        }
        needFree_b = true;
    }
    else
    {
        /* Zero fill (truncate extension) */
        srcBuf_pu8 = calloc(1u, len_u8);
        if (srcBuf_pu8 == NULL)
            return TEE_ERROR_OUT_OF_MEMORY;
        needFree_b = true;
    }

    mutex_lock(&g_lock_L);

    lfs_file_seek(&g_lfs_L, &h_ps->file_s, (lfs_soff_t)pos, LFS_SEEK_SET);

    written_i32 = lfs_file_write(&g_lfs_L, &h_ps->file_s, srcBuf_pu8, len_u8);
    if (written_i32 < 0 || (size_t)written_i32 != len_u8)
    {
        res = TEE_ERROR_GENERIC;
    }
    else
    {
        /* Flush to flash — OP-TEE expects durability after each write */
        lfs_file_sync(&g_lfs_L, &h_ps->file_s);

        /* Update cached size */
        lfs_soff_t newSize_i32 = lfs_file_size(&g_lfs_L, &h_ps->file_s);

        if (newSize_i32 >= 0)
            h_ps->dataSize_u32 = (uint32_t)newSize_i32;
    }

    mutex_unlock(&g_lock_L);

    if (needFree_b)
        free(srcBuf_pu8);

    return res;
}

static TEE_Result tee_lfs_truncate_L(struct tee_file_handle *fh, size_t size_u32)
{
    struct tee_lfs_fh *h_ps = (struct tee_lfs_fh *)fh;
    int lfsRes_i32 = 0;

    mutex_lock(&g_lock_L);

    lfsRes_i32 = lfs_file_truncate(&g_lfs_L, &h_ps->file_s, (lfs_off_t)size_u32);
    if (lfsRes_i32 == LFS_ERR_OK)
    {
        lfs_file_sync(&g_lfs_L, &h_ps->file_s);
        h_ps->dataSize_u32 = (uint32_t)size_u32;
    }

    mutex_unlock(&g_lock_L);
    return lfs_to_tee_err_L(lfsRes_i32);
}

static TEE_Result tee_lfs_remove_L(struct tee_pobj *po)
{
    char path_au8[TEE_LFS_PATH_MAX];
    int lfsRes_i32 = 0;
    /*
     * Bitmap of in-use blocks: 1 bit per block (15360 blocks = 1920 bytes each).
     * Kept static (not on the stack) to avoid a ~3.8 KB stack burst on large
     * volumes; safe because the whole function runs under g_lock_L.
     */
    static uint8_t beforeBitmap_au8[(W77Q_LFS_BLOCK_COUNT + 7U) / 8U];
    static uint8_t afterBitmap_au8[(W77Q_LFS_BLOCK_COUNT + 7U) / 8U];

    build_path_L(po, path_au8);

    mutex_lock(&g_lock_L);

    /* Record blocks in use BEFORE removal */
    memset(beforeBitmap_au8, 0, sizeof(beforeBitmap_au8));
    lfs_fs_traverse(&g_lfs_L, mark_block_cb_L, beforeBitmap_au8);

    /* Remove the file (LFS frees its blocks but doesn't erase them) */
    lfsRes_i32 = lfs_remove(&g_lfs_L, path_au8);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        mutex_unlock(&g_lock_L);
        return lfs_to_tee_err_L(lfsRes_i32);
    }

    /* Record blocks in use AFTER removal */
    memset(afterBitmap_au8, 0, sizeof(afterBitmap_au8));
    lfs_fs_traverse(&g_lfs_L, mark_block_cb_L, afterBitmap_au8);

    /* Zero freed blocks via raw flash write (NOR bit-clear: 1→0 is valid) */
    {
        uint32_t blk_u32 = 0;
        uint8_t zeros_au8[W77Q_LFS_PROG_SIZE];

        memset(zeros_au8, 0, sizeof(zeros_au8));

        for (blk_u32 = 0; blk_u32 < W77Q_LFS_BLOCK_COUNT; blk_u32++)
        {
            bool wasUsed_b = (beforeBitmap_au8[blk_u32 / 8U] >>
                              (blk_u32 % 8U)) & 1U;
            bool stillUsed_b = (afterBitmap_au8[blk_u32 / 8U] >>
                                (blk_u32 % 8U)) & 1U;

            if (wasUsed_b && !stillUsed_b)
            {
                /* This block was freed — zero its entire 4KB content */
                uint32_t addr_u32 = blk_u32 * W77Q_LFS_BLOCK_SIZE;
                uint32_t off_u32 = 0;

                for (off_u32 = 0; off_u32 < W77Q_LFS_BLOCK_SIZE;
                     off_u32 += W77Q_LFS_PROG_SIZE)
                {
                    W77Q_LFS_PORT_WriteRaw(addr_u32 + off_u32,
                                           zeros_au8, W77Q_LFS_PROG_SIZE);
                }
            }
        }
    }

    mutex_unlock(&g_lock_L);
    return TEE_SUCCESS;
}

static TEE_Result tee_lfs_rename_L(struct tee_pobj *old_po, struct tee_pobj *new_po,
                                   bool overwrite_b)
{
    char oldPath_au8[TEE_LFS_PATH_MAX];
    char newPath_au8[TEE_LFS_PATH_MAX];
    int lfsRes_i32 = 0;
    TEE_Result res = TEE_SUCCESS;

    build_path_L(old_po, oldPath_au8);
    build_path_L(new_po, newPath_au8);

    mutex_lock(&g_lock_L);

    /* Check if destination exists */
    if (!overwrite_b)
    {
        struct lfs_info info_s;

        if (lfs_stat(&g_lfs_L, newPath_au8, &info_s) == LFS_ERR_OK)
        {
            mutex_unlock(&g_lock_L);
            return TEE_ERROR_ACCESS_CONFLICT;
        }
    }
    else
    {
        /* Zeroize target before overwrite */
        zeroize_file_L(newPath_au8);
        lfs_remove(&g_lfs_L, newPath_au8);
    }

    /* Ensure target TA directory exists */
    res = ensure_ta_dir_L(&new_po->uuid);
    if (res != TEE_SUCCESS)
    {
        mutex_unlock(&g_lock_L);
        return res;
    }

    lfsRes_i32 = lfs_rename(&g_lfs_L, oldPath_au8, newPath_au8);

    mutex_unlock(&g_lock_L);
    return lfs_to_tee_err_L(lfsRes_i32);
}

/* ---------------------------------------------------------------------------
 * Directory enumeration
 * --------------------------------------------------------------------------*/

static TEE_Result tee_lfs_opendir_L(const TEE_UUID *uuid,
                                    struct tee_fs_dir **out_dir)
{
    struct tee_fs_dir *d_ps = NULL;
    int lfsRes_i32 = 0;

    if (uuid == NULL || out_dir == NULL)
        return TEE_ERROR_BAD_PARAMETERS;

    d_ps = calloc(1u, sizeof(*d_ps));
    if (d_ps == NULL)
        return TEE_ERROR_OUT_OF_MEMORY;

    d_ps->uuid = *uuid;
    build_dir_path_L(uuid, d_ps->dirPath_au8);

    mutex_lock(&g_lock_L);

    lfsRes_i32 = lfs_dir_open(&g_lfs_L, &d_ps->dir_s, d_ps->dirPath_au8);
    if (lfsRes_i32 != LFS_ERR_OK)
    {
        mutex_unlock(&g_lock_L);
        free(d_ps);
        /* No directory = no objects for this TA — return empty iterator */
        if (lfsRes_i32 == LFS_ERR_NOENT)
        {
            d_ps = calloc(1u, sizeof(*d_ps));
            if (d_ps == NULL)
                return TEE_ERROR_OUT_OF_MEMORY;
            d_ps->uuid = *uuid;
            d_ps->dirPath_au8[0] = '\0'; /* mark as "empty" */
            *out_dir = d_ps;
            return TEE_SUCCESS;
        }
        return lfs_to_tee_err_L(lfsRes_i32);
    }

    mutex_unlock(&g_lock_L);
    *out_dir = d_ps;
    return TEE_SUCCESS;
}

static TEE_Result tee_lfs_readdir_L(struct tee_fs_dir *d_ps,
                                    struct tee_fs_dirent **out_ent)
{
    struct lfs_info info_s;
    int lfsRes_i32 = 0;

    if (d_ps == NULL || out_ent == NULL)
        return TEE_ERROR_BAD_PARAMETERS;

    /* Empty iterator (TA directory doesn't exist) */
    if (d_ps->dirPath_au8[0] == '\0')
        return TEE_ERROR_ITEM_NOT_FOUND;

    mutex_lock(&g_lock_L);

    while ((lfsRes_i32 = lfs_dir_read(&g_lfs_L, &d_ps->dir_s, &info_s)) > 0)
    {
        size_t objIdLen_u32 = 0;

        /* Skip directories, ".", ".." */
        if (info_s.type != LFS_TYPE_REG)
            continue;

        /* Decode obj_id from hex filename */
        objIdLen_u32 = hex_to_bytes_L(info_s.name, d_ps->curr.oid,
                                      TEE_OBJECT_ID_MAX_LEN);
        if (objIdLen_u32 == 0)
            continue;

        d_ps->curr.oidlen = (uint32_t)objIdLen_u32;
        *out_ent = &d_ps->curr;

        mutex_unlock(&g_lock_L);
        return TEE_SUCCESS;
    }

    mutex_unlock(&g_lock_L);
    return TEE_ERROR_ITEM_NOT_FOUND;
}

static void tee_lfs_closedir_L(struct tee_fs_dir *d_ps)
{
    if (d_ps != NULL)
    {
        if (d_ps->dirPath_au8[0] != '\0')
        {
            mutex_lock(&g_lock_L);
            lfs_dir_close(&g_lfs_L, &d_ps->dir_s);
            mutex_unlock(&g_lock_L);
        }
        free(d_ps);
    }
}
