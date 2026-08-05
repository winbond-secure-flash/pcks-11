/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       qlib_platform.c
* @brief      QLIB platform callbacks for OP-TEE Secure World (self-contained SPI driver)
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/

/*-----------------------------------------------------------------------------------------------------------
                                                   INCLUDES
-----------------------------------------------------------------------------------------------------------*/

#include <arm.h>
#include <crypto/crypto.h>
#include <io.h>
#include <kernel/panic.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <string.h>
#include <tee_api_defines.h>
#include <trace.h>
#include <types_ext.h>
#include <util.h>

#include "qlib_platform.h"

/*-----------------------------------------------------------------------------------------------------------
                                                 DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

#define SHA256_DIGEST_SIZE       32U
#define BITS_PER_BYTE            8U
#define SPI_DBG_MAX_BYTES        8U
#define ADDR_SIZE_4BYTE          4U
#define ADDR_SIZE_3BYTE          3U
#define ADDR_SIZE_2BYTE          2U
#define SPI_DBG_BYTE2_IDX        2U
#define SPI_DBG_BYTE3_IDX        3U

/* ---- Board-specific: R-Car V4H RPC-IF addresses ---- */
#define RPCIF_PHYS_BASE          0xEE200000U  /* RPC-IF register block */
#define RPCIF_FLASH_PHYS_BASE    0x08000000U  /* Flash data window (XIP) */

/* ---- RPC-IF register offsets ---- */
#define RPCIF_CMNCR              0x000U
#define RPCIF_SSLDR              0x004U
#define RPCIF_DRCR               0x00CU
#define RPCIF_DRCMR              0x010U
#define RPCIF_DREAR              0x014U
#define RPCIF_DRENR              0x01CU
#define RPCIF_SMCR               0x020U
#define RPCIF_SMENR              0x030U
#define RPCIF_SMRDR0             0x038U
#define RPCIF_SMWDR0             0x040U
#define RPCIF_CMNSR              0x048U
#define RPCIF_DRDMCR             0x058U
#define RPCIF_DRDRENR            0x05CU
#define RPCIF_SMDRENR            0x064U
#define RPCIF_PHYCNT             0x07CU
#define RPCIF_PHYOFFSET1         0x080U
#define RPCIF_PHYOFFSET2         0x084U
#define RPCIF_MMIO_SIZE          0x0A0U

/* CMNCR bits */
#define CMNCR_MD                 BIT(31)
#define CMNCR_SFDE               BIT(24)
#define CMNCR_MOIIO3(v)          (((v) & 0x3U) << 22)
#define CMNCR_MOIIO2(v)          (((v) & 0x3U) << 20)
#define CMNCR_MOIIO1(v)          (((v) & 0x3U) << 18)
#define CMNCR_MOIIO0(v)          (((v) & 0x3U) << 16)
#define CMNCR_MOIIO_HIZ          (CMNCR_MOIIO0(3) | CMNCR_MOIIO1(3) | \
                                  CMNCR_MOIIO2(3) | CMNCR_MOIIO3(3))
#define CMNCR_IO3FV(v)           (((v) & 0x3U) << 14)
#define CMNCR_IO2FV(v)           (((v) & 0x3U) << 12)
#define CMNCR_IO0FV(v)           (((v) & 0x3U) << 8)
#define CMNCR_IOFV_HIZ           (CMNCR_IO0FV(3) | CMNCR_IO2FV(3) | CMNCR_IO3FV(3))
#define CMNCR_BSZ(v)             (((v) & 0x3U) << 0)

/* SSLDR bits */
#define SSLDR_SPNDL(v)           (((v) & 0x7U) << 16)
#define SSLDR_SLNDL(v)           (((v) & 0x7U) << 8)
#define SSLDR_SCKDL(v)           (((v) & 0x7U) << 0)

/* PHYCNT bits (R-Car Gen4) */
#define PHYCNT_CAL               BIT(31)
#define PHYCNT_GEN4_STRTIM       ((7U << 15) | (1U << 27))
#define PHYCNT_GEN4_UNDOC        0x260U

/* PHYOFFSET1 / PHYOFFSET2 */
#define PHYOFFSET1_DDRTMG(v)     (((v) & 0x3U) << 28)
#define PHYOFFSET2_OCTTMG(v)     (((v) & 0x7U) << 8)

/* SMCR bits */
#define SMCR_SSLKP               BIT(8)
#define SMCR_SPIRE               BIT(2)
#define SMCR_SPIWE               BIT(1)
#define SMCR_SPIE                BIT(0)

/* SMENR bits */
#define SMENR_SPIDE_8BIT         0x8U

/* CMNSR bits */
#define CMNSR_TEND               BIT(0)

/* DRCR bits */
#define DRCR_SSLN                BIT(24)
#define DRCR_RBURST(v)           (((v) & 0x1FU) << 16)
#define DRCR_RCF                 BIT(9)
#define DRCR_RBE                 BIT(8)
#define DRCR_SSLE                BIT(0)

/* DRCMR / DRENR bits */
#define DRCMR_CMD(v)             (((uint32_t)(v) & 0xFFU) << 16)
#define DRENR_CDE                BIT(14)
#define DRENR_DME                BIT(15)
#define DRENR_ADE(v)             (((uint32_t)(v) & 0xFU) << 8)

#define RPCIF_TEND_TIMEOUT_US    10000U
#define RPCIF_PHY_CAL_RETRIES    10000U

/* Flash data window size (64 MB) */
#define RPCIF_FLASH_SIZE         0x04000000U

/* SPI transfer flags (match spi.h) */
#define SPI_XFER_BEGIN           BIT(0)
#define SPI_XFER_END             BIT(1)

/*-----------------------------------------------------------------------------------------------------------
                                                  TYPES
-----------------------------------------------------------------------------------------------------------*/

/* Internal RPC-IF state — replaces the external rcar_rpcif struct. */
typedef struct RPCIF_STATE_T
{
    vaddr_t  baseVa;         ///< MMIO register base virtual address
    vaddr_t  flashVa;        ///< Flash data window virtual address (XIP)
} RPCIF_STATE_T;

/*-----------------------------------------------------------------------------------------------------------
                                              LOCAL VARIABLES
-----------------------------------------------------------------------------------------------------------*/

/* Pre-register MMIO and flash-window regions so phys_to_virt resolves. */
register_phys_mem_pgdir(MEM_AREA_IO_SEC, RPCIF_PHYS_BASE, RPCIF_MMIO_SIZE);
register_phys_mem_pgdir(MEM_AREA_IO_SEC, RPCIF_FLASH_PHYS_BASE, RPCIF_FLASH_SIZE);

static RPCIF_STATE_T g_rpcif_L;
static bool          g_spiInited_b_L;
static bool          g_busClaimed_b_L;

/*-----------------------------------------------------------------------------------------------------------
                                         LOCAL FUNCTION PROTOTYPES
-----------------------------------------------------------------------------------------------------------*/

static uint32_t   rpcifRead_L(uint32_t off_u32);
static void       rpcifWrite_L(uint32_t off_u32, uint32_t val_u32);
static void       rpcifFlushCache_L(void);
static TEE_Result rpcifWaitTend_L(void);
static TEE_Result rpcifClaim_L(void);
static TEE_Result rpcifInit_L(void);
static TEE_Result rpcifXfer_L(const uint8_t *tx_pu8, uint8_t *rx_pu8,
                              uint32_t len_u32, uint32_t flags_u32);
static TEE_Result rpcifFlashRead_L(uint32_t flashAddr_u32, uint8_t *buf_pu8, uint32_t len_u32);
static TEE_Result rpcifCmdRead_L(uint8_t cmd_u8, uint32_t addr_u32,
                                 uint32_t addrNibbles_u32, uint32_t dummyCycles_u32,
                                 uint8_t *buf_pu8, uint32_t len_u32);
static void       ensureSpiReady_L(void);

/*-----------------------------------------------------------------------------------------------------------
                                             INTERFACE FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

/* -------------------------------------------------------------------------
 * SPI callbacks
 * ---------------------------------------------------------------------- */

void PLAT_SPI_MultiTransactionStart(void)
{
    ensureSpiReady_L();
    if (rpcifClaim_L() != TEE_SUCCESS)
        panic("QLIB: rpcifClaim_L failed");
    g_busClaimed_b_L = true;
}

void PLAT_SPI_MultiTransactionStop(void)
{
    g_busClaimed_b_L = false;
}

int32_t PLAT_SPI_WriteReadTransaction(const void     *userData_pv __unused,
                                      QLIB_BUS_MODE_T format __unused,
                                      uint32_t        flags_u32 __unused,
                                      const uint8_t  *dataOutStream_pu8,
                                      uint32_t        cmdSize_u32,
                                      uint32_t        addressSize_u32,
                                      uint32_t        dataOutSize_u32,
                                      uint32_t        dummyCycles_u32,
                                      uint8_t        *dataIn_pu8,
                                      uint32_t        dataInSize_u32)
{
    uint32_t totalTx_u32 = cmdSize_u32 + addressSize_u32 + dataOutSize_u32;
    TEE_Result res = TEE_SUCCESS;

    ensureSpiReady_L();

    DMSG("PLAT_SPI: cmdSz=%u addrSz=%u doutSz=%u dummy=%u dinSz=%u cmd0=0x%02x",
         cmdSize_u32, addressSize_u32, dataOutSize_u32, dummyCycles_u32, dataInSize_u32,
         (dataOutStream_pu8 != NULL && cmdSize_u32 > 0U) ? dataOutStream_pu8[0] : 0xFFU);

    /* Single transactions: claim the bus (restores manual SPI mode). */
    if (!g_busClaimed_b_L)
    {
        res = rpcifClaim_L();
        if (res != TEE_SUCCESS)
        {
            EMSG("QLIB: rpcifClaim_L failed: %#" PRIx32, res);
            return -1;
        }
    }

    /* ---- RX path: dataInSize_u32 > 0 ---- */
    if (dataInSize_u32 > 0U)
    {
        if (addressSize_u32 > 0U && dummyCycles_u32 > 0U)
        {
            /*
             * QLIB proprietary command with address + dummy cycles
             * (e.g. cmd=0x85, 3-byte address).  Use direct-mode read.
             */
            uint32_t qlibAddr_u32 = 0U;
            uint32_t addrNibbles_u32 = 0U;
            uint32_t i = 0U;

            for (i = 0U; i < addressSize_u32 && i < ADDR_SIZE_4BYTE; i++)
            {
                qlibAddr_u32 = (qlibAddr_u32 << BITS_PER_BYTE) |
                               dataOutStream_pu8[cmdSize_u32 + i];
            }

            if (addressSize_u32 == ADDR_SIZE_4BYTE)
                addrNibbles_u32 = 0xFU;
            else if (addressSize_u32 == ADDR_SIZE_3BYTE)
                addrNibbles_u32 = 0x7U;
            else if (addressSize_u32 == ADDR_SIZE_2BYTE)
                addrNibbles_u32 = 0x3U;
            else
                addrNibbles_u32 = 0x1U;

            res = rpcifCmdRead_L(dataOutStream_pu8[0], qlibAddr_u32,
                                 addrNibbles_u32, dummyCycles_u32,
                                 dataIn_pu8, dataInSize_u32);
            return (res != TEE_SUCCESS) ? -1 : 0;
        }

        if (addressSize_u32 > 0U)
        {
            /*
             * Standard flash read: CMD + ADDR -> DATA (no dummy).
             * Uses XIP mode to work around SMRDR0 errata.
             */
            uint32_t flashAddr_u32 = 0U;
            uint32_t i = 0U;

            for (i = 0U; i < addressSize_u32 && i < ADDR_SIZE_4BYTE; i++)
            {
                flashAddr_u32 = (flashAddr_u32 << BITS_PER_BYTE) |
                                dataOutStream_pu8[cmdSize_u32 + i];
            }

            res = rpcifFlashRead_L(flashAddr_u32, dataIn_pu8, dataInSize_u32);
            return (res != TEE_SUCCESS) ? -1 : 0;
        }

        /*
         * Command-response read (no address): RDID, RDSR, QLIB OP0/OP2.
         * Use direct-mode with no address phase.
         */
        if (cmdSize_u32 == 1U && dataOutSize_u32 == 0U)
        {
            res = rpcifCmdRead_L(dataOutStream_pu8[0], 0U, 0U,
                                 dummyCycles_u32, dataIn_pu8, dataInSize_u32);
            if (res == TEE_SUCCESS && dataInSize_u32 <= SPI_DBG_MAX_BYTES)
            {
                uint32_t b0_u32 = dataInSize_u32 > 0U ? dataIn_pu8[0] : 0xFFU;
                uint32_t b1_u32 = dataInSize_u32 > 1U ? dataIn_pu8[1] : 0xFFU;
                uint32_t b2_u32 = dataInSize_u32 > SPI_DBG_BYTE2_IDX ? dataIn_pu8[SPI_DBG_BYTE2_IDX] : 0xFFU;
                uint32_t b3_u32 = dataInSize_u32 > SPI_DBG_BYTE3_IDX ? dataIn_pu8[SPI_DBG_BYTE3_IDX] : 0xFFU;

                DMSG("QLIB SPI rx: cmd=%02x dummy=%u [%02x %02x %02x %02x]",
                     dataOutStream_pu8[0], dummyCycles_u32,
                     b0_u32, b1_u32, b2_u32, b3_u32);
            }
            return (res != TEE_SUCCESS) ? -1 : 0;
        }

        /*
         * Fallback for multi-byte command headers or combined TX+RX.
         * Uses manual-mode byte-at-a-time (SMRDR0 errata: RX=0x00).
         */
        {
            bool csActive_b = false;

            if (totalTx_u32 > 0U)
            {
                res = rpcifXfer_L(dataOutStream_pu8, NULL,
                                  totalTx_u32, SPI_XFER_BEGIN);
                if (res != TEE_SUCCESS)
                    return -1;
                csActive_b = true;
            }

            if (dummyCycles_u32 > 0U)
            {
                static const uint8_t ff_u8 = 0xFFU;
                uint32_t dummyBytes_u32 = (dummyCycles_u32 + 7U) / 8U;
                uint32_t j = 0U;

                for (j = 0U; j < dummyBytes_u32; j++)
                {
                    uint32_t xf_u32 = csActive_b ? 0U : SPI_XFER_BEGIN;

                    csActive_b = true;
                    res = rpcifXfer_L(&ff_u8, NULL, 1U, xf_u32);
                    if (res != TEE_SUCCESS)
                        return -1;
                }
            }

            {
                uint32_t xferFlags_u32 = SPI_XFER_END;

                if (!csActive_b)
                    xferFlags_u32 |= SPI_XFER_BEGIN;
                res = rpcifXfer_L(NULL, dataIn_pu8, dataInSize_u32, xferFlags_u32);
            }
        }
        return (res != TEE_SUCCESS) ? -1 : 0;
    }

    /* ---- TX-only path: WREN, PP, SE, session commands ---- */
    if (totalTx_u32 > 0U)
    {
        uint32_t xferFlags_u32 = SPI_XFER_BEGIN;

        if (dummyCycles_u32 == 0U)
            xferFlags_u32 |= SPI_XFER_END;

        res = rpcifXfer_L(dataOutStream_pu8, NULL, totalTx_u32, xferFlags_u32);
        if (res != TEE_SUCCESS)
            return -1;
    }

    /* Dummy-byte clocking after TX (round up to whole bytes). */
    if (dummyCycles_u32 > 0U)
    {
        static const uint8_t dummyFf_au8[8] = {
            0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
        };
        uint32_t remaining_u32 = (dummyCycles_u32 + 7U) / 8U;
        bool first_b = (totalTx_u32 == 0U);

        while (remaining_u32 > 0U)
        {
            uint32_t chunk_u32 = MIN(remaining_u32, (uint32_t)sizeof(dummyFf_au8));
            uint32_t xferFlags_u32 = 0U;

            if (first_b)
            {
                xferFlags_u32 |= SPI_XFER_BEGIN;
                first_b = false;
            }
            if (remaining_u32 <= chunk_u32)
                xferFlags_u32 |= SPI_XFER_END;

            res = rpcifXfer_L(dummyFf_au8, NULL, chunk_u32, xferFlags_u32);
            if (res != TEE_SUCCESS)
                return -1;
            remaining_u32 -= chunk_u32;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * SHA-256 callbacks (TEE_ALG_SHA256 = 0x50000004)
 * ---------------------------------------------------------------------- */

int32_t PLAT_HASH_Init(void **ctx_ppv, QLIB_HASH_OPT_T opt __unused)
{
    TEE_Result res = crypto_hash_alloc_ctx(ctx_ppv, TEE_ALG_SHA256);

    if (res != TEE_SUCCESS)
        return -1;

    res = crypto_hash_init(*ctx_ppv);
    if (res != TEE_SUCCESS)
    {
        crypto_hash_free_ctx(*ctx_ppv);
        *ctx_ppv = NULL;
        return -1;
    }
    return 0;
}

int32_t PLAT_HASH_Update(void *ctx_pv, const void *data_pv, uint32_t dataSize_u32)
{
    return (crypto_hash_update(ctx_pv, (const uint8_t *)data_pv, dataSize_u32) != TEE_SUCCESS)
           ? -1 : 0;
}

int32_t PLAT_HASH_Finish(void *ctx_pv, uint32_t *output_pu32)
{
    /*
     * QLIB API contract: output_pu32 must point to uint32_t[8] (32 bytes).
     * The caller (libqlib.a) always provides a SHA-256-sized buffer.
     * We cannot add a size parameter — the signature is fixed by QLIB.
     */
    _Static_assert(SHA256_DIGEST_SIZE == 32U,
                   "SHA256_DIGEST_SIZE changed — update PLAT_HASH_Finish buffer assumption");

    uint8_t digest_au8[SHA256_DIGEST_SIZE] = { 0 };
    TEE_Result res = crypto_hash_final(ctx_pv, digest_au8, sizeof(digest_au8));

    if (res == TEE_SUCCESS)
        memcpy(output_pu32, digest_au8, sizeof(digest_au8));
    crypto_hash_free_ctx(ctx_pv);
    return (res != TEE_SUCCESS) ? -1 : 0;
}

/* -------------------------------------------------------------------------
 * Nonce / RNG callbacks
 * ---------------------------------------------------------------------- */

uint64_t PLAT_GetNONCE(void)
{
    uint64_t nonce_u64 = 0U;

    if (crypto_rng_read(&nonce_u64, sizeof(nonce_u64)) != TEE_SUCCESS)
        panic("QLIB: PLAT_GetNONCE crypto_rng_read failed");
    return nonce_u64;
}

int32_t PLAT_SetNONCE(uint64_t constNonce_u64 __unused)
{
    /* Fixed nonce for testing only — not permitted in production. */
    return -1;
}

/* -------------------------------------------------------------------------
 * Reset callback
 * ---------------------------------------------------------------------- */

void CORE_RESET(void)
{
    panic("QLIB: CORE_RESET");
}

/*-----------------------------------------------------------------------------------------------------------
                                              LOCAL FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

static uint32_t rpcifRead_L(uint32_t off_u32)
{
    return io_read32(g_rpcif_L.baseVa + off_u32);
}

static void rpcifWrite_L(uint32_t off_u32, uint32_t val_u32)
{
    io_write32(g_rpcif_L.baseVa + off_u32, val_u32);
}

static void rpcifFlushCache_L(void)
{
    rpcifWrite_L(RPCIF_DRCR,
                 DRCR_SSLN | DRCR_RBURST(31) | DRCR_RCF | DRCR_RBE | DRCR_SSLE);
    (void)rpcifRead_L(RPCIF_DRCR); /* ensure write commits */
}

static TEE_Result rpcifWaitTend_L(void)
{
    uint32_t retries_u32 = RPCIF_TEND_TIMEOUT_US;

    while ((rpcifRead_L(RPCIF_CMNSR) & CMNSR_TEND) == 0U)
    {
        if (retries_u32 == 0U)
            return TEE_ERROR_BUSY;
        retries_u32--;
        isb();
    }
    return TEE_SUCCESS;
}

/*
 * rpcifClaim_L() - Re-initialise PHY and restore manual SPI mode.
 * Call before each flash operation to undo any XIP/Linux interference.
 */
static TEE_Result rpcifClaim_L(void)
{
    uint32_t i = 0U;

    /* PHY calibration */
    rpcifWrite_L(RPCIF_PHYOFFSET1,
                 rpcifRead_L(RPCIF_PHYOFFSET1) | PHYOFFSET1_DDRTMG(3));
    rpcifWrite_L(RPCIF_PHYOFFSET2,
                 (rpcifRead_L(RPCIF_PHYOFFSET2) & ~PHYOFFSET2_OCTTMG(7)) |
                 PHYOFFSET2_OCTTMG(4));
    rpcifWrite_L(RPCIF_PHYCNT,
                 PHYCNT_CAL | PHYCNT_GEN4_STRTIM | PHYCNT_GEN4_UNDOC);

    for (i = 0U; i < RPCIF_PHY_CAL_RETRIES; i++)
    {
        if ((rpcifRead_L(RPCIF_PHYCNT) & PHYCNT_CAL) == 0U)
            break;
        isb();
    }
    if (i >= RPCIF_PHY_CAL_RETRIES)
        EMSG("rpcifClaim_L: PHY calibration timeout");

    /* Restore manual SPI mode (MD=1) */
    rpcifWrite_L(RPCIF_CMNCR, CMNCR_MD | CMNCR_SFDE |
                 CMNCR_MOIIO_HIZ | CMNCR_IOFV_HIZ | CMNCR_BSZ(0));
    rpcifWrite_L(RPCIF_SSLDR,
                 SSLDR_SPNDL(7) | SSLDR_SLNDL(7) | SSLDR_SCKDL(7));
    rpcifWrite_L(RPCIF_SMDRENR, 0U);
    rpcifFlushCache_L();

    return TEE_SUCCESS;
}

/*
 * rpcifInit_L() - Map MMIO, map flash window, run PHY init sequence.
 * Called once on first SPI access (lazy init).
 */
static TEE_Result rpcifInit_L(void)
{
    vaddr_t va = (vaddr_t)phys_to_virt_io((paddr_t)RPCIF_PHYS_BASE, RPCIF_MMIO_SIZE);
    vaddr_t fva = 0U;
    uint32_t i = 0U;

    if (va == 0U)
    {
        EMSG("qlib_platform: MMIO mapping failed for PA 0x%lx", (unsigned long)RPCIF_PHYS_BASE);
        return TEE_ERROR_GENERIC;
    }

    fva = (vaddr_t)phys_to_virt_io((paddr_t)RPCIF_FLASH_PHYS_BASE, RPCIF_FLASH_SIZE);
    if (fva == 0U)
    {
        EMSG("qlib_platform: flash window mapping failed");
        return TEE_ERROR_GENERIC;
    }

    g_rpcif_L.baseVa  = va;
    g_rpcif_L.flashVa = fva;

    /* PHY init: PHYOFFSET1 DDRTMG=3, PHYOFFSET2 OCTTMG=4, trigger CAL */
    rpcifWrite_L(RPCIF_PHYOFFSET1,
                 rpcifRead_L(RPCIF_PHYOFFSET1) | PHYOFFSET1_DDRTMG(3));
    rpcifWrite_L(RPCIF_PHYOFFSET2,
                 (rpcifRead_L(RPCIF_PHYOFFSET2) & ~PHYOFFSET2_OCTTMG(7)) |
                 PHYOFFSET2_OCTTMG(4));
    rpcifWrite_L(RPCIF_PHYCNT,
                 PHYCNT_CAL | PHYCNT_GEN4_STRTIM | PHYCNT_GEN4_UNDOC);

    for (i = 0U; i < RPCIF_PHY_CAL_RETRIES; i++)
    {
        if ((rpcifRead_L(RPCIF_PHYCNT) & PHYCNT_CAL) == 0U)
            break;
        isb();
    }
    if (i >= RPCIF_PHY_CAL_RETRIES)
    {
        EMSG("qlib_platform: PHY calibration timeout");
        return TEE_ERROR_BUSY;
    }

    /* Manual SPI mode (MD=1), SDR, flush cache */
    rpcifWrite_L(RPCIF_CMNCR, CMNCR_MD | CMNCR_SFDE |
                 CMNCR_MOIIO_HIZ | CMNCR_IOFV_HIZ | CMNCR_BSZ(0));
    rpcifWrite_L(RPCIF_SMDRENR, 0U);
    rpcifFlushCache_L();
    rpcifWrite_L(RPCIF_SSLDR,
                 SSLDR_SPNDL(7) | SSLDR_SLNDL(7) | SSLDR_SCKDL(7));

    IMSG("qlib_platform: RPC-IF ready at PA 0x%lx CMNCR=0x%08x",
         (unsigned long)RPCIF_PHYS_BASE, rpcifRead_L(RPCIF_CMNCR));

    return TEE_SUCCESS;
}

/*
 * rpcifXfer_L() - Transfer len_u32 bytes in manual SPI mode (byte-at-a-time).
 */
static TEE_Result rpcifXfer_L(const uint8_t *tx_pu8, uint8_t *rx_pu8,
                              uint32_t len_u32, uint32_t flags_u32)
{
    TEE_Result res = TEE_SUCCESS;
    uint32_t i = 0U;

    if (len_u32 == 0U)
        return TEE_SUCCESS;

    for (i = 0U; i < len_u32; i++)
    {
        bool last_b = (i == len_u32 - 1U);
        uint32_t smcr_u32 = SMCR_SPIE;

        if (tx_pu8 != NULL)
            smcr_u32 |= SMCR_SPIWE;
        if (rx_pu8 != NULL)
            smcr_u32 |= SMCR_SPIRE;
        if (tx_pu8 == NULL && rx_pu8 == NULL)
            smcr_u32 |= SMCR_SPIWE;

        if (!last_b || (flags_u32 & SPI_XFER_END) == 0U)
            smcr_u32 |= SMCR_SSLKP;

        res = rpcifWaitTend_L();
        if (res != TEE_SUCCESS)
        {
            EMSG("rpcifXfer_L: TEND pre-wait timeout at byte %u", i);
            return res;
        }

        rpcifWrite_L(RPCIF_SMWDR0,
                     (uint32_t)((tx_pu8 != NULL) ? ((uint32_t)tx_pu8[i] << 24) : 0xFF000000U));
        rpcifWrite_L(RPCIF_SMENR, SMENR_SPIDE_8BIT);
        rpcifWrite_L(RPCIF_SMCR, smcr_u32);

        res = rpcifWaitTend_L();
        if (res != TEE_SUCCESS)
        {
            EMSG("rpcifXfer_L: TEND timeout at byte %u", i);
            return res;
        }

        if (rx_pu8 != NULL)
        {
            uint32_t raw_u32 = rpcifRead_L(RPCIF_SMRDR0);

            rx_pu8[i] = (uint8_t)((raw_u32 >> 24) & 0xFFU);
        }
    }

    return TEE_SUCCESS;
}

/*
 * rpcifFlashRead_L() - Read from flash via XIP (direct-read) mode.
 * Works around SMRDR0 silicon errata on R-Car Gen4.
 */
static TEE_Result rpcifFlashRead_L(uint32_t flashAddr_u32, uint8_t *buf_pu8, uint32_t len_u32)
{
    uint32_t i = 0U;

    if (len_u32 == 0U)
        return TEE_SUCCESS;

    if (flashAddr_u32 + len_u32 < flashAddr_u32 ||
        flashAddr_u32 + len_u32 > RPCIF_FLASH_SIZE)
        return TEE_ERROR_BAD_PARAMETERS;

    /* Switch to XIP mode (MD=0), flush, configure direct-read path */
    rpcifWrite_L(RPCIF_CMNCR,
                 CMNCR_SFDE | CMNCR_MOIIO_HIZ | CMNCR_IOFV_HIZ | CMNCR_BSZ(0));
    rpcifFlushCache_L();

    /* Standard READ (0x03): command + 3-byte address, SDR, no dummy */
    rpcifWrite_L(RPCIF_DRCMR, 0U);
    rpcifWrite_L(RPCIF_DRCMR, DRCMR_CMD(0x03));
    rpcifWrite_L(RPCIF_DREAR, 0U);
    rpcifWrite_L(RPCIF_DRDRENR, 0U);
    rpcifWrite_L(RPCIF_DRENR, DRENR_CDE | DRENR_ADE(0x7));

    /* Copy from memory-mapped flash window */
    for (i = 0U; i < len_u32; i++)
        buf_pu8[i] = io_read8(g_rpcif_L.flashVa + flashAddr_u32 + i);

    /* Restore manual SPI mode */
    rpcifWrite_L(RPCIF_CMNCR,
                 CMNCR_MD | CMNCR_SFDE | CMNCR_MOIIO_HIZ |
                 CMNCR_IOFV_HIZ | CMNCR_BSZ(0));
    rpcifFlushCache_L();

    return TEE_SUCCESS;
}

/*
 * rpcifCmdRead_L() - Direct-mode read: CMD [+ ADDR] [+ dummy] -> DATA.
 * Generalised read for QLIB proprietary commands.
 */
static TEE_Result rpcifCmdRead_L(uint8_t cmd_u8, uint32_t addr_u32,
                                 uint32_t addrNibbles_u32, uint32_t dummyCycles_u32,
                                 uint8_t *buf_pu8, uint32_t len_u32)
{
    uint32_t drenr_u32 = DRENR_CDE;
    uint32_t i = 0U;

    if (addr_u32 + len_u32 < addr_u32 ||
        addr_u32 + len_u32 > RPCIF_FLASH_SIZE)
        return TEE_ERROR_BAD_PARAMETERS;

    /* Switch to direct mode (MD=0) — DRCMR/DRENR only latch when MD=0 */
    rpcifWrite_L(RPCIF_CMNCR,
                 CMNCR_SFDE | CMNCR_MOIIO_HIZ | CMNCR_IOFV_HIZ | CMNCR_BSZ(0));
    rpcifFlushCache_L();

    /*
     * Disable burst pre-fetch for address-phase commands (single-byte
     * response per CMD+ADDR transaction).
     */
    if (addrNibbles_u32 != 0U)
        rpcifWrite_L(RPCIF_DRCR, DRCR_SSLN | DRCR_SSLE);

    /* Configure command, address enable, dummy cycles */
    rpcifWrite_L(RPCIF_DRCMR,   DRCMR_CMD(cmd_u8));
    rpcifWrite_L(RPCIF_DREAR,   0U);
    rpcifWrite_L(RPCIF_DRDRENR, 0U);

    if (addrNibbles_u32 != 0U)
        drenr_u32 |= DRENR_ADE(addrNibbles_u32);
    if (dummyCycles_u32 > 0U)
    {
        rpcifWrite_L(RPCIF_DRDMCR, (dummyCycles_u32 - 1U) & 0x1FU);
        drenr_u32 |= DRENR_DME;
    }
    rpcifWrite_L(RPCIF_DRENR, drenr_u32);

    /* Read from flash window at addr offset */
    for (i = 0U; i < len_u32; i++)
        buf_pu8[i] = io_read8(g_rpcif_L.flashVa + addr_u32 + i);

    /* Restore manual SPI mode */
    rpcifWrite_L(RPCIF_CMNCR,
                 CMNCR_MD | CMNCR_SFDE | CMNCR_MOIIO_HIZ |
                 CMNCR_IOFV_HIZ | CMNCR_BSZ(0));
    rpcifFlushCache_L();

    return TEE_SUCCESS;
}

/*
 * ensureSpiReady_L() - Lazy-init: perform RPC-IF hardware init on first call.
 */
static void ensureSpiReady_L(void)
{
    if (!g_spiInited_b_L)
    {
        if (rpcifInit_L() != TEE_SUCCESS)
            panic("QLIB: RPC-IF SPI init failed");
        g_spiInited_b_L = true;
    }
}
