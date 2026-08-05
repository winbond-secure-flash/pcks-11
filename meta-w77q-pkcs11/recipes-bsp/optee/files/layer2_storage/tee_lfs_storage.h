/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       tee_lfs_storage.h
* @brief      LittleFS-based secure storage backend for OP-TEE on W77Q flash
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef TEE_LFS_STORAGE_H__
#define TEE_LFS_STORAGE_H__

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------------------------------------------
                                                 INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include "lfs.h"
#include "w77q_lfs_port.h"

#include <stdint.h>
#include <tee/tee_fs.h>
#include <tee_api_types.h>

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

/* Maximum path length: "/" + 32-char UUID hex + "/" + 128-char obj_id hex + NUL */
#define TEE_LFS_PATH_MAX        164U

/*-----------------------------------------------------------------------------------------------------------
                                                  TYPES
-----------------------------------------------------------------------------------------------------------*/

/** File handle — wraps an open LittleFS file and its per-file cache buffer. */
struct tee_lfs_fh
{
    lfs_file_t file_s;                               ///< LittleFS file descriptor
    struct lfs_file_config fileCfg_s;                ///< Per-file config (holds buffer ptr)
    uint8_t fileBuf_au8[W77Q_LFS_CACHE_SIZE];        ///< Static per-file cache (LFS_NO_MALLOC), must == cache_size
    char path_au8[TEE_LFS_PATH_MAX];                ///< Full LFS path for this object
    uint32_t dataSize_u32;                           ///< Cached file size (bytes)
};

/** Directory enumeration handle. */
struct tee_fs_dir
{
    lfs_dir_t dir_s;                                 ///< LittleFS directory descriptor
    TEE_UUID uuid;                                   ///< TA UUID filter
    char dirPath_au8[34];                            ///< "/<uuid_hex>" (1 + 32 + NUL)
    struct tee_fs_dirent curr;                       ///< Current dirent for readdir
};

/*-----------------------------------------------------------------------------------------------------------
                                            INTERFACE FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

/************************************************************************************************************
 * @brief       Initialise the LittleFS storage backend.
 *
 * Calls W77Q_LFS_PORT_HwInit() then attempts lfs_mount().
 * On first boot (or corrupted FS), calls lfs_format() + lfs_mount().
 *
 * @return
 * TEE_SUCCESS              - backend ready\n
 * TEE_ERROR_GENERIC        - hardware or FS initialisation failure
 ************************************************************************************************************/
TEE_Result tee_lfs_init(void);

/* tee_lfs_is_ready() and tee_lfs_ops are declared in <tee/tee_fs.h> (guarded by
 * CFG_W77Q_FS) — do not redeclare here to avoid -Wredundant-decls. */

/************************************************************************************************************
 * @brief       Enumerate every stored object regardless of TA UUID.
 *
 * @param[in]    cb              Callback invoked per object; non-zero return stops iteration
 * @param[in]    ctx_pv          Opaque pointer forwarded to cb
 ************************************************************************************************************/
typedef int32_t (*tee_lfs_list_all_cb)(const uint8_t ta_uuid_u8[16],
                                      uint32_t flash_off_u32,
                                      uint32_t data_size_u32,
                                      const uint8_t *obj_id_u8,
                                      uint32_t obj_id_len_u32,
                                      void *ctx_pv);

void tee_lfs_list_all(tee_lfs_list_all_cb cb, void *ctx_pv);

/************************************************************************************************************
 * @brief       Read bytes directly from W77Q flash (bypasses LittleFS).
 ************************************************************************************************************/
TEE_Result tee_lfs_read_raw(uint32_t off_u32, void *buf_pv, size_t len_u32);

/************************************************************************************************************
 * @brief       Read W77Q flash without authenticated session (diagnostic).
 ************************************************************************************************************/
TEE_Result tee_lfs_read_plain(uint32_t off_u32, void *buf_pv, size_t len_u32);

/************************************************************************************************************
 * @brief       Read a file's content through LittleFS by TA UUID and object ID.
 *
 * @param[in]    ta_uuid_u8      16-byte TA UUID
 * @param[in]    obj_id_pu8      Object ID bytes
 * @param[in]    obj_id_len_u32  Length of obj_id_pu8
 * @param[out]   buf_pv          Output buffer
 * @param[in]    buf_size_u32    Size of buf_pv
 * @param[out]   out_size_pu32   Actual bytes read (may be NULL)
 *
 * @return
 * TEE_SUCCESS              - file read successfully\n
 * TEE_ERROR_ITEM_NOT_FOUND - file does not exist\n
 * TEE_ERROR_GENERIC        - read error
 ************************************************************************************************************/
TEE_Result tee_lfs_read_file(const uint8_t ta_uuid_u8[16],
                             const char *obj_id_pu8, size_t obj_id_len_u32,
                             void *buf_pv, size_t buf_size_u32,
                             size_t *out_size_pu32);

/************************************************************************************************************
 * @brief       Write raw bytes to W77Q flash (bypasses LittleFS).
 *
 * WARNING: Corrupts LittleFS metadata.  Caller must reformat or reboot.
 ************************************************************************************************************/
TEE_Result tee_lfs_write_raw(uint32_t off_u32, const void *buf_pv, size_t len_u32);

/************************************************************************************************************
 * @brief       Erase the 4 KB sector containing off_u32 (bypasses LittleFS).
 *
 * WARNING: Corrupts LittleFS metadata.  Remounts FS after erase.
 ************************************************************************************************************/
TEE_Result tee_lfs_erase_sector(uint32_t off_u32);

/************************************************************************************************************
 * @brief       Erase all of section 1, reformat and remount LittleFS.
 ************************************************************************************************************/
TEE_Result tee_lfs_erase_chip(void);

#ifdef __cplusplus
}
#endif

#endif /* TEE_LFS_STORAGE_H__ */
