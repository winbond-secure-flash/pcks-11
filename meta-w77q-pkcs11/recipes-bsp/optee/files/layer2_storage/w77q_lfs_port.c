/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_lfs_port.c
* @brief      LittleFS block-device port and raw flash access for W77Q via QLIB
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/

// SPDX-License-Identifier: BSD-2-Clause

/*-----------------------------------------------------------------------------------------------------------
                                                 INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include "w77q_lfs_port.h"

#include <drivers/rcar_rpcif.h>
#include <drivers/w77q.h>
#include <string.h>
#include <trace.h>

/* Declared in w77q_qlib.c — plain (unauthenticated) read for diagnostic. */
TEE_Result w77q_read_plain(struct w77q *dev, uint32_t addr_u32, void *buf_pv, size_t len);

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/
#ifndef CFG_RPCIF_BASE
#define CFG_RPCIF_BASE 0xEE200000U
#endif

#define W77Q_LFS_ZERO_CHUNK 256U    ///< Chunk size for secure-erase zero writes
#define W77Q_LFS_WRITE_CHUNK 256U   ///< Max page-program size for raw writes
#define W77Q_LFS_ERASE_SECTOR 4096U ///< Physical NOR erase-sector granularity (4 KB)

/*-----------------------------------------------------------------------------------------------------------
                                              LOCAL VARIABLES
-----------------------------------------------------------------------------------------------------------*/
static struct rcar_rpcif g_rpcif_L;
static struct w77q g_w77q_L;

static uint8_t g_readBuf_au8_L[W77Q_LFS_CACHE_SIZE];
static uint8_t g_progBuf_au8_L[W77Q_LFS_CACHE_SIZE];
static uint8_t g_lookaheadBuf_au8_L[W77Q_LFS_LOOKAHEAD_SIZE];

/*-----------------------------------------------------------------------------------------------------------
                                          LOCAL FUNCTION PROTOTYPES
-----------------------------------------------------------------------------------------------------------*/
static int W77Q_LFS_PORT_Read_L(const struct lfs_config *cfg_ps, lfs_block_t block_u32,
                                lfs_off_t off_u32, void *buf_pv, lfs_size_t size_u32);
static int W77Q_LFS_PORT_Prog_L(const struct lfs_config *cfg_ps, lfs_block_t block_u32,
                                lfs_off_t off_u32, const void *buf_pv, lfs_size_t size_u32);
static int W77Q_LFS_PORT_Erase_L(const struct lfs_config *cfg_ps, lfs_block_t block_u32);
static int W77Q_LFS_PORT_Sync_L(const struct lfs_config *cfg_ps);

/*-----------------------------------------------------------------------------------------------------------
                                              LOCAL FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

/*
 * OP-TEE's kernel libc does not provide strcspn/strspn.
 * LittleFS needs them for path separator parsing.
 */
size_t strcspn(const char *s_pu8, const char *reject_pu8)
{
    const char *p_pu8 = s_pu8;

    while (*p_pu8 != '\0')
    {
        const char *r_pu8 = reject_pu8;

        while (*r_pu8 != '\0')
        {
            if (*p_pu8 == *r_pu8)
                return (size_t)(p_pu8 - s_pu8);
            r_pu8++;
        }
        p_pu8++;
    }
    return (size_t)(p_pu8 - s_pu8);
}

size_t strspn(const char *s_pu8, const char *accept_pu8)
{
    const char *p_pu8 = s_pu8;

    while (*p_pu8 != '\0')
    {
        const char *a_pu8 = accept_pu8;
        bool found_b = false;

        while (*a_pu8 != '\0')
        {
            if (*p_pu8 == *a_pu8)
            {
                found_b = true;
                break;
            }
            a_pu8++;
        }
        if (!found_b)
            return (size_t)(p_pu8 - s_pu8);
        p_pu8++;
    }
    return (size_t)(p_pu8 - s_pu8);
}

/*-----------------------------------------------------------------------------------------------------------
                                            INTERFACE FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

TEE_Result W77Q_LFS_PORT_HwInit(void)
{
    TEE_Result res = TEE_SUCCESS;

    res = rcar_rpcif_init(&g_rpcif_L, (paddr_t)CFG_RPCIF_BASE);
    if (res != TEE_SUCCESS)
    {
        EMSG("w77q_lfs_port: RPC-IF init failed: %#" PRIx32, res);
        return res;
    }

    res = w77q_init(&g_w77q_L, &g_rpcif_L.chip);
    if (res != TEE_SUCCESS)
    {
        EMSG("w77q_lfs_port: W77Q init failed: %#" PRIx32, res);
        return res;
    }

    IMSG("w77q_lfs_port: W77Q init OK - size=0x%zx", g_w77q_L.size);
    return TEE_SUCCESS;
}

void W77Q_LFS_PORT_Init(struct lfs_config *cfg_ps)
{
    memset(cfg_ps, 0, sizeof(*cfg_ps));

    cfg_ps->context = &g_w77q_L;

    /* Block device callbacks */
    cfg_ps->read  = W77Q_LFS_PORT_Read_L;
    cfg_ps->prog  = W77Q_LFS_PORT_Prog_L;
    cfg_ps->erase = W77Q_LFS_PORT_Erase_L;
    cfg_ps->sync  = W77Q_LFS_PORT_Sync_L;

    /* Flash geometry */
    cfg_ps->read_size      = W77Q_LFS_READ_SIZE;
    cfg_ps->prog_size      = W77Q_LFS_PROG_SIZE;
    cfg_ps->block_size     = W77Q_LFS_BLOCK_SIZE;
    cfg_ps->block_count    = W77Q_LFS_BLOCK_COUNT;
    cfg_ps->cache_size     = W77Q_LFS_CACHE_SIZE;
    cfg_ps->lookahead_size = W77Q_LFS_LOOKAHEAD_SIZE;
    cfg_ps->block_cycles   = W77Q_LFS_BLOCK_CYCLES;

    /*
     * Storage policy: pack small files (LittleFS default inlining).
     *
     * Small files (<= inline_max) are stored inside the shared directory
     * metadata block instead of a dedicated CTZ data block, so several objects
     * share one 4 KB block. This gives the best space efficiency and the lowest
     * flash wear, and it removes the per-login write churn caused by forcing
     * every object into its own block.
     *
     * Security trade-off (accepted): deleting an inlined file does NOT free a
     * whole block, so the immediate block-diff zeroization in tee_lfs_remove_L()
     * does not scrub it. The plaintext lingers physically until LittleFS
     * compacts that metadata block (eventual wipe, no timing guarantee). Objects
     * large enough to own a CTZ data block are still scrubbed immediately on
     * delete.
     *
     * inline_max is derived from the block size (W77Q_LFS_INLINE_MAX =
     * min(cache_size, attr_max, block_size/8), capped at 1022). For 4K blocks
     * this is 512; for 8K blocks it is 1022. Larger inline_max keeps a bigger
     * token.db inlined (see W77Q_LFS_BLOCK_SIZE guidance). lfs_init() asserts
     * inline_max <= cache_size, <= attr_max, <= block_size/8.
     */
    cfg_ps->inline_max     = W77Q_LFS_INLINE_MAX;

    /* Static buffers (LFS_NO_MALLOC) */
    cfg_ps->read_buffer      = g_readBuf_au8_L;
    cfg_ps->prog_buffer      = g_progBuf_au8_L;
    cfg_ps->lookahead_buffer = g_lookaheadBuf_au8_L;
}

/* ---------------------------------------------------------------------------
 * Raw flash access (diagnostic / dump PTA)
 * --------------------------------------------------------------------------*/

TEE_Result W77Q_LFS_PORT_ReadRaw(uint32_t off_u32, void *buf_pv, size_t len_u32)
{
    return w77q_read(&g_w77q_L, off_u32, buf_pv, len_u32);
}

TEE_Result W77Q_LFS_PORT_ReadPlain(uint32_t off_u32, void *buf_pv, size_t len_u32)
{
    return w77q_read_plain(&g_w77q_L, off_u32, buf_pv, len_u32);
}

TEE_Result W77Q_LFS_PORT_WriteRaw(uint32_t off_u32, const void *buf_pv, size_t len_u32)
{
    const uint8_t *p_pu8 = buf_pv;
    size_t remaining_u32 = len_u32;
    TEE_Result res = TEE_SUCCESS;

    if (len_u32 == 0)
        return TEE_SUCCESS;

    while (remaining_u32 > 0)
    {
        size_t chunk_u32 = (remaining_u32 > W77Q_LFS_WRITE_CHUNK)
                            ? W77Q_LFS_WRITE_CHUNK : remaining_u32;

        res = w77q_write(&g_w77q_L, off_u32, p_pu8, chunk_u32);
        if (res != TEE_SUCCESS)
            break;
        p_pu8        += chunk_u32;
        off_u32      += (uint32_t)chunk_u32;
        remaining_u32 -= chunk_u32;
    }
    return res;
}

TEE_Result W77Q_LFS_PORT_EraseSector(uint32_t off_u32)
{
    return w77q_erase_sector(&g_w77q_L, off_u32);
}

TEE_Result W77Q_LFS_PORT_ErasePartition(void)
{
    TEE_Result res = TEE_SUCCESS;
    uint32_t off_u32 = 0;

    IMSG("w77q_lfs_port: erasing entire partition (%u sectors)",
         (unsigned)(W77Q_LFS_SECTION_SIZE / W77Q_LFS_BLOCK_SIZE));

    for (off_u32 = 0; off_u32 < W77Q_LFS_SECTION_SIZE; off_u32 += W77Q_LFS_BLOCK_SIZE)
    {
        res = w77q_erase_sector(&g_w77q_L, off_u32);
        if (res != TEE_SUCCESS)
        {
            EMSG("w77q_lfs_port: erase failed at 0x%08" PRIx32 ": %#" PRIx32,
                 off_u32, res);
            return res;
        }
    }
    return TEE_SUCCESS;
}

/*-----------------------------------------------------------------------------------------------------------
                                              LOCAL FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

/************************************************************************************************************
 * @internal
 * @brief       Read a region within one block from W77Q flash.
 * @endinternal
 ************************************************************************************************************/
static int W77Q_LFS_PORT_Read_L(const struct lfs_config *cfg_ps, lfs_block_t block_u32,
                                lfs_off_t off_u32, void *buf_pv, lfs_size_t size_u32)
{
    struct w77q *dev_ps = (struct w77q *)cfg_ps->context;
    uint32_t addr_u32 = (block_u32 * W77Q_LFS_BLOCK_SIZE) + off_u32;
    TEE_Result res = w77q_read(dev_ps, addr_u32, buf_pv, size_u32);

    return (res == TEE_SUCCESS) ? LFS_ERR_OK : LFS_ERR_IO;
}

/************************************************************************************************************
 * @internal
 * @brief       Program a region within one block on W77Q flash.
 * @endinternal
 ************************************************************************************************************/
static int W77Q_LFS_PORT_Prog_L(const struct lfs_config *cfg_ps, lfs_block_t block_u32,
                                lfs_off_t off_u32, const void *buf_pv, lfs_size_t size_u32)
{
    struct w77q *dev_ps = (struct w77q *)cfg_ps->context;
    uint32_t addr_u32 = (block_u32 * W77Q_LFS_BLOCK_SIZE) + off_u32;
    const uint8_t *p_pu8 = buf_pv;
    lfs_size_t remaining_u32 = size_u32;
    TEE_Result res = TEE_SUCCESS;

    /*
     * LittleFS may flush up to cache_size (currently 512) bytes in one prog
     * call, but the W77Q page-program limit is W77Q_LFS_WRITE_CHUNK (256).
     * Split here so the port stays correct regardless of cache_size or which
     * w77q_write backend is linked (QLIB chunks internally, but the raw
     * RPC-IF driver rejects len > 256).
     */
    while (remaining_u32 > 0)
    {
        lfs_size_t chunk_u32 = (remaining_u32 > W77Q_LFS_WRITE_CHUNK)
                                ? W77Q_LFS_WRITE_CHUNK : remaining_u32;

        res = w77q_write(dev_ps, addr_u32, p_pu8, chunk_u32);
        if (res != TEE_SUCCESS)
            return LFS_ERR_IO;

        p_pu8        += chunk_u32;
        addr_u32      += chunk_u32;
        remaining_u32 -= chunk_u32;
    }

    return LFS_ERR_OK;
}

/************************************************************************************************************
 * @internal
 * @brief       Erase one LittleFS block with secure zeroize.
 *
 * Writes all-zeros over the entire block BEFORE calling the hardware erase.
 * NOR flash allows bit-clear (1->0) without erase, so writing zeros destroys
 * residual data.  A LittleFS block may span several 4 KB flash sectors
 * (block_size is a multiple of W77Q_LFS_ERASE_SECTOR), so every sector in the
 * block is erased back to 0xFF.
 * @endinternal
 ************************************************************************************************************/
static int W77Q_LFS_PORT_Erase_L(const struct lfs_config *cfg_ps, lfs_block_t block_u32)
{
    struct w77q *dev_ps = (struct w77q *)cfg_ps->context;
    uint32_t addr_u32 = block_u32 * W77Q_LFS_BLOCK_SIZE;
    uint8_t zeros_au8[W77Q_LFS_ZERO_CHUNK];
    uint32_t done_u32 = 0;
    TEE_Result res = TEE_SUCCESS;

    memset(zeros_au8, 0, sizeof(zeros_au8));

    /* Secure zeroize: write all-zeros before erase */
    while (done_u32 < W77Q_LFS_BLOCK_SIZE)
    {
        uint32_t chunk_u32 = W77Q_LFS_BLOCK_SIZE - done_u32;

        if (chunk_u32 > W77Q_LFS_ZERO_CHUNK)
        {
            chunk_u32 = W77Q_LFS_ZERO_CHUNK;
        }

        res = w77q_write(dev_ps, addr_u32 + done_u32, zeros_au8, chunk_u32);
        if (res != TEE_SUCCESS)
        {
            EMSG("w77q_lfs_port: zeroize failed at 0x%08" PRIx32 ": %#" PRIx32,
                 addr_u32 + done_u32, res);
            return LFS_ERR_IO;
        }
        done_u32 += chunk_u32;
    }

    /* Hardware erase (sets flash back to 0xFF). A LittleFS block may span more
       than one 4 KB physical sector, so erase every sector it covers. */
    for (done_u32 = 0; done_u32 < W77Q_LFS_BLOCK_SIZE; done_u32 += W77Q_LFS_ERASE_SECTOR)
    {
        res = w77q_erase_sector(dev_ps, addr_u32 + done_u32);
        if (res != TEE_SUCCESS)
        {
            EMSG("w77q_lfs_port: erase failed at 0x%08" PRIx32 ": %#" PRIx32,
                 addr_u32 + done_u32, res);
            return LFS_ERR_IO;
        }
    }

    return LFS_ERR_OK;
}

/************************************************************************************************************
 * @internal
 * @brief       Sync — no-op for W77Q (SPI writes are synchronous).
 * @endinternal
 ************************************************************************************************************/
static int W77Q_LFS_PORT_Sync_L(const struct lfs_config *cfg_ps)
{
    (void)cfg_ps;
    return LFS_ERR_OK;
}