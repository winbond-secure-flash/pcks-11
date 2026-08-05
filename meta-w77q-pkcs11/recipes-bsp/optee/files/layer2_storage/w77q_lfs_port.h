/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_lfs_port.h
* @brief      LittleFS block-device port and raw flash access for W77Q via QLIB
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef W77Q_LFS_PORT_H__
#define W77Q_LFS_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------------------------------------------
                                                 INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include "lfs.h"

#include <stdint.h>
#include <tee_api_types.h>

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/
#define W77Q_LFS_READ_SIZE      32U                             ///< QLIB secure read page (QLIB_SEC_READ_PAGE_SIZE_BYTE)
#define W77Q_LFS_PROG_SIZE      32U                             ///< QLIB secure write page (QLIB_SEC_WRITE_PAGE_SIZE_BYTE)

/* Section 1 size limit (16 MB — per-section maximum; matches W77Q_CFG_SECTION1_SIZE_MB in w77q_config.h). */
#define W77Q_LFS_SECTION_SIZE   ((uint32_t)(16U * 1024U * 1024U))

/*
 * LittleFS erase-block size — the single configuration knob.
 *
 * token.db is stored inline while it fits in block_size/8 (hard-capped at
 * 1022 B by LittleFS attr_max), and it grows ~16 B per PKCS#11 object:
 *   - For object count 20-50 use 8192U (8K) — keeps token.db inlined.
 *   - For fewer, or more than ~50, objects use 4096U (4K).
 * Changing block size requires resetting all PKCS#11 data (re-format the
 * partition) — the new geometry is not compatible with an existing layout.
 *
 * All dependent geometry below is derived automatically from this value.
 */
#define W77Q_LFS_BLOCK_SIZE     4096U

#define W77Q_LFS_BLOCK_COUNT    (W77Q_LFS_SECTION_SIZE / W77Q_LFS_BLOCK_SIZE)    ///< 16 MB / block_size
#define W77Q_LFS_CACHE_SIZE     (W77Q_LFS_BLOCK_SIZE / 8U)                       ///< Read/prog cache (= block_size/8)
#define W77Q_LFS_INLINE_MAX     (((W77Q_LFS_BLOCK_SIZE / 8U) > 1022U)          \
                                     ? 1022U : (W77Q_LFS_BLOCK_SIZE / 8U))       ///< Max inline file (attr_max cap = 1022)
#define W77Q_LFS_LOOKAHEAD_SIZE 64U                                             ///< Block allocator bitmap (bytes)
#define W77Q_LFS_BLOCK_CYCLES   500                                             ///< Wear-leveling trigger threshold

/* Compile-time geometry checks (mirror the LittleFS lfs_init asserts). */
#if ((16U * 1024U * 1024U) % W77Q_LFS_BLOCK_SIZE) != 0
#error "W77Q_LFS_BLOCK_SIZE must evenly divide the 16 MB section"
#endif
#if (W77Q_LFS_BLOCK_SIZE % 4096U) != 0
#error "W77Q_LFS_BLOCK_SIZE must be a multiple of the 4 KB flash sector"
#endif
#if ((W77Q_LFS_BLOCK_SIZE / 8U) % W77Q_LFS_PROG_SIZE) != 0
#error "cache_size (block_size/8) must be a multiple of W77Q_LFS_PROG_SIZE"
#endif

/*-----------------------------------------------------------------------------------------------------------
                                            INTERFACE FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

/************************************************************************************************************
 * @brief       Initialise RPC-IF and W77Q hardware.
 *
 * Must be called before W77Q_LFS_PORT_Init().
 *
 * @return
 * TEE_SUCCESS              - hardware ready\n
 * TEE_ERROR_GENERIC        - RPC-IF or W77Q init failure
 ************************************************************************************************************/
TEE_Result W77Q_LFS_PORT_HwInit(void);

/************************************************************************************************************
 * @brief       Populate an lfs_config struct with W77Q flash callbacks and geometry.
 *
 * All static buffers (read cache, prog cache, lookahead) are internal to
 * the port module.  The caller must not free or reallocate them.
 * W77Q_LFS_PORT_HwInit() must have been called first.
 *
 * @param[out]   cfg_ps          LittleFS config struct to populate
 ************************************************************************************************************/
void W77Q_LFS_PORT_Init(struct lfs_config *cfg_ps);

/************************************************************************************************************
 * @brief       Read bytes directly from W77Q flash (authenticated, bypasses LittleFS).
 ************************************************************************************************************/
TEE_Result W77Q_LFS_PORT_ReadRaw(uint32_t off_u32, void *buf_pv, size_t len_u32);

/************************************************************************************************************
 * @brief       Read W77Q flash without authenticated session (diagnostic only).
 ************************************************************************************************************/
TEE_Result W77Q_LFS_PORT_ReadPlain(uint32_t off_u32, void *buf_pv, size_t len_u32);

/************************************************************************************************************
 * @brief       Write raw bytes to W77Q flash (authenticated, bypasses LittleFS).
 *
 * WARNING: Corrupts LittleFS metadata.  Caller must reformat or reboot.
 ************************************************************************************************************/
TEE_Result W77Q_LFS_PORT_WriteRaw(uint32_t off_u32, const void *buf_pv, size_t len_u32);

/************************************************************************************************************
 * @brief       Erase the 4 KB sector at the given offset (bypasses LittleFS).
 ************************************************************************************************************/
TEE_Result W77Q_LFS_PORT_EraseSector(uint32_t off_u32);

/************************************************************************************************************
 * @brief       Erase all sectors in the section 1 partition (bypasses LittleFS).
 ************************************************************************************************************/
TEE_Result W77Q_LFS_PORT_ErasePartition(void);

#ifdef __cplusplus
}
#endif

#endif /* W77Q_LFS_PORT_H__ */
