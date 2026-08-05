/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_qlib.c
* @brief      W77Q flash driver backed by the full QLIB stack in OP-TEE
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
// SPDX-License-Identifier: BSD-2-Clause

/*-----------------------------------------------------------------------------------------------------------
                                                INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include <crypto/crypto.h>
#include <drivers/w77q.h>
#include <string.h>
#include <tee_api_types.h>
#include <trace.h>
#include <types_ext.h>
#include <util.h>

#include "qlib.h"
#include "w77q_config.h"

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

/* QLIB context - must be non-static so qlib_platform.c can reference. */
QLIB_CONTEXT_T g_qlib_ctx;

/* Section 1 is the OP-TEE secure storage section. */
#define W77Q_STORAGE_SECTION W77Q_CFG_STORAGE_SECTION
#define W77Q_SECTION0_SIZE_MB W77Q_CFG_SECTION0_SIZE_MB
#define W77Q_SECTION1_SIZE_MB W77Q_CFG_SECTION1_SIZE_MB

/*-----------------------------------------------------------------------------------------------------------
                                             LOCAL VARIABLES
-----------------------------------------------------------------------------------------------------------*/

/* True once OpenSession on section 1 succeeds. */
static bool g_sec1_ready_b_L;

/* SHA-256 digest size */
#define SHA256_DIGEST_SIZE 32U

#define SPI_EXTCFG_MAX_DUMMY        30U
#define FLASH_CR_DUMMY_DEFAULT      8U

#define QLIB_WINBOND_KD1            W77Q_CFG_KD1

#define QLIB_WINBOND_KD             W77Q_CFG_KD

/*
 * derive_section_key_L() - Derive a 128-bit section key from WID + label.
 * FK = SHA-256(WID[8] || label)[0:16]
 */
static TEE_Result derive_section_key_L(const uint8_t wid_u8[8], const char *label, uint8_t out_key_u8[16])
{
    uint8_t digest_u8[SHA256_DIGEST_SIZE] = {};
    void *ctx_pv = NULL;
    TEE_Result res = TEE_SUCCESS;

    res = crypto_hash_alloc_ctx(&ctx_pv, TEE_ALG_SHA256);
    if (res)
        return res;

    res = crypto_hash_init(ctx_pv);
    if (!res)
        res = crypto_hash_update(ctx_pv, wid_u8, 8u);
    if (!res)
        res = crypto_hash_update(ctx_pv, (const uint8_t *)label, strlen(label));
    if (!res)
        res = crypto_hash_final(ctx_pv, digest_u8, SHA256_DIGEST_SIZE);

    crypto_hash_free_ctx(ctx_pv);
    if (!res)
        memcpy(out_key_u8, digest_u8, 16u);
    return res;
}

QLIB_STATUS_T get_device_key(QLIB_CONTEXT_T* ctx_pv, QLIB_KID_TYPE_T keyType, KEY_T deviceKey)
{
    if (keyType == QLIB_KID__DEVICE_KEY_PRE_PROVISIONING)
    {
        if (QLIB_FeatureSupported(ctx_pv, W77Q_FEATURE_PRE_PROV_MASTER_KEY) != (bool)false)
        {
            KEY_T key = QLIB_WINBOND_KD1;
            (void)memcpy(deviceKey, key, sizeof(KEY_T));
        }
        else
        {
            (void)memset(deviceKey, 0, sizeof(KEY_T));
        }
    }
    else if (keyType == QLIB_KID__DEVICE_MASTER)
    {
        KEY_T key = QLIB_WINBOND_KD;
        (void)memcpy(deviceKey, key, sizeof(KEY_T));
    }
    else
    {
        return QLIB_STATUS__INVALID_PARAMETER;
    }
    return QLIB_STATUS__OK;
}

TEE_Result w77q_configChip(void)
{
    QLIB_FLASH_CONFIG_T           flashCfg = {0};
    QLIB_DIE_CONFIG_T             dieCfg   = {0};
    QLIB_SECTIONS_CONF_TABLE_T    cfgLine[QLIB_MAX_DIES_SUPPORTED];
    QLIB_EXTENDED_SECTION_CONF_T* sectCfg;
    uint8_t                       diesNum_u8 = QLIB_GET_NUM_OF_DIES(&g_qlib_ctx);
    uint8_t                       die_u8     = 0u;
    TEE_Result                    res      = TEE_SUCCESS;
    QLIB_STATUS_T                 status   = QLIB_STATUS__OK;
    QLIB_ID_T                     id       = {0};
    KEY_T                         fk       = {0};
    KEY_T                         rk       = {0};

    IMSG("w77q_qlib: >>> w77q_configChip enter (dies=%u)", diesNum_u8);

    IMSG("w77q_qlib: QLIB_GetDeviceConfigMultiDie ...");
    status = QLIB_GetDeviceConfigMultiDie(&g_qlib_ctx, &flashCfg, &dieCfg, cfgLine, diesNum_u8);
    if (status != QLIB_STATUS__OK)
    {
        EMSG("Error: QLIB_GetDeviceConfigMultiDie failed (0x%X)\n", status);
        return TEE_ERROR_GENERIC;
    }
    IMSG("w77q_qlib: QLIB_GetDeviceConfigMultiDie OK (dies=%u) --- full config dump:", diesNum_u8);

    /* --- flashCfg (die 0 fields) --- */
    IMSG("w77q_qlib: flashCfg.watchdogDefault:"
         " enable=%d_u8 lfOscEn=%d_u8 swResetEn=%d_u8 authenticated=%d_u8"
         " sectionID=%u threshold=%d_u8 lock=%d_u8 fallbackEn=%d_u8 oscRateHz=%u",
         flashCfg.watchdogDefault.enable,
         flashCfg.watchdogDefault.lfOscEn,
         flashCfg.watchdogDefault.swResetEn,
         flashCfg.watchdogDefault.authenticated,
         flashCfg.watchdogDefault.sectionID,
         (int32_t)flashCfg.watchdogDefault.threshold,
         flashCfg.watchdogDefault.lock,
         flashCfg.watchdogDefault.fallbackEn,
         flashCfg.watchdogDefault.oscRateHz);
    IMSG("w77q_qlib: flashCfg.pinMux:"
         " io23Mux=%d_u8 dedicatedResetInEn=%d_u8",
         (int32_t)flashCfg.pinMux.io23Mux,
         flashCfg.pinMux.dedicatedResetInEn);
    IMSG("w77q_qlib: flashCfg.stdAddrSize:"
         " addrLen=%d_u8 sidLen=%d_u8 addrMode=%d_u8",
         (int32_t)flashCfg.stdAddrSize.addrLen_u8,
         (int32_t)flashCfg.stdAddrSize.sidLen_u8,
         (int32_t)flashCfg.stdAddrSize.addrMode);
    IMSG("w77q_qlib: flashCfg:"
         " fastReadDummyCycles=%u dqsDisable=%d_u8",
         flashCfg.fastReadDummyCycles,
         flashCfg.dqsDisable);

    /* --- dieCfg --- */

    IMSG("w77q_qlib: dieCfg:"
         " safeFB=%d_u8 FBFail=%d_u8 FBSelect=%d_u8 FBMapping=%u"
         " speculCK=%d_u8 nonSecureFormatEn=%d_u8 rngPAEn=%d_u8"
         " ctagModeMulti=%d_u8 devcfgLock=%d_u8",
         dieCfg.safeFB, dieCfg.FBFail, dieCfg.FBSelect, dieCfg.FBMapping,
         dieCfg.speculCK, dieCfg.nonSecureFormatEn, dieCfg.rngPAEn,
         dieCfg.ctagModeMulti, dieCfg.devcfgLock);

    /* --- cfgLine: per-die, per-section --- */
    for (uint8_t d_u8 = 0; d_u8 < diesNum_u8; d_u8++)
    {
        QLIB_EXTENDED_SECTION_CONF_T *tbl = cfgLine[d_u8].sectionConfigTable;
        for (uint8_t s_u8 = 0; s_u8 < QLIB_NUM_OF_SECTIONS; s_u8++)
        {
            IMSG("w77q_qlib: cfgLine[die_u8=%u][sec=%u]:"
                 " base=0x%08x size=0x%08x resetPA=%d_u8 crc=0x%08x ver=%u",
                 d_u8, s_u8,
                 tbl[s_u8].baseAddr, tbl[s_u8].size, tbl[s_u8].resetPA,
                 tbl[s_u8].sectionConfig.crc, tbl[s_u8].sectionConfig.version);
            IMSG("w77q_qlib:   policy:"
                 " plain_r=%d_u8 plain_w=%d_u8 auth=%d_u8 csum=%d_u8 dig=%d_u8"
                 " digOnAccess=%d_u8 rollback=%d_u8 writeProt=%d_u8 slog=%d_u8",
                 tbl[s_u8].sectionConfig.policy.plainAccessReadEnable,
                 tbl[s_u8].sectionConfig.policy.plainAccessWriteEnable,
                 tbl[s_u8].sectionConfig.policy.authPlainAccess,
                 tbl[s_u8].sectionConfig.policy.checksumIntegrity,
                 tbl[s_u8].sectionConfig.policy.digestIntegrity,
                 tbl[s_u8].sectionConfig.policy.digestIntegrityOnAccess,
                 tbl[s_u8].sectionConfig.policy.rollbackProt,
                 tbl[s_u8].sectionConfig.policy.writeProt,
                 tbl[s_u8].sectionConfig.policy.slog);
        }
    }
    IMSG("w77q_qlib: --- end config dump ---");

    // TBD: - Check that chip is already configured and return if so, to avoid unnecessary re-provisioning on every boot.

    IMSG("w77q_qlib: QLIB_GetId (in configChip) ...");
    status = QLIB_GetId(&g_qlib_ctx, &id);
    if (status != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_GetId failed: %u - cannot derive keys", status);
        return TEE_ERROR_GENERIC;
    }
    IMSG("w77q_qlib: QLIB_GetId OK");
    /*-------------------------------------------------------------------------------------------------------
     Step 2: Set device keys (pre-provisioning key + master key)
    -------------------------------------------------------------------------------------------------------*/
    IMSG("Setting device keys...\n");
    status = get_device_key(&g_qlib_ctx,
                            QLIB_KID__DEVICE_KEY_PRE_PROVISIONING,
                            dieCfg.preProvisionedMasterKey);
    if (status != QLIB_STATUS__OK)
    {
        EMSG("Error: GetDeviceKey(PRE_PROV) failed (0x%X)\n", status);
        return TEE_ERROR_GENERIC;
    }
    IMSG("w77q_qlib: GetDeviceKey(PRE_PROV) OK");
    status = get_device_key(&g_qlib_ctx,
                            QLIB_KID__DEVICE_MASTER,
                            dieCfg.deviceMasterKey);
    if (status != QLIB_STATUS__OK)
    {
        EMSG("Error: GetDeviceKey(MASTER) failed (0x%X)\n", status);
        return TEE_ERROR_GENERIC;
    }
    IMSG("w77q_qlib: GetDeviceKey(MASTER) OK");
    /*-------------------------------------------------------------------------------------------------------
     Step 3: Configure sections for FW update (rollback-protected)
       Section 1 -- symmetric-key demo
       Section 2 -- asymmetric-key (LMS) demo
    -------------------------------------------------------------------------------------------------------*/
#if QLIB_MAX_DIES_SUPPORTED > 1
    QLIB_GetActiveDie(&g_qlib_ctx, &die_u8);
#endif  // QLIB_MAX_DIES_SUPPORTED > 1
    sectCfg = cfgLine[die_u8].sectionConfigTable;
    /* --- Section 0: Boot section --- */
    IMSG("Configuring section %u for symmetric\n", 0u);
    sectCfg[0].baseAddr = 0;
    sectCfg[0].size     = W77Q_SECTION0_SIZE_MB * 1024U * 1024U;
    sectCfg[0].sectionConfig.policy.plainAccessReadEnable   = 1;
    sectCfg[0].sectionConfig.policy.plainAccessWriteEnable  = 0;
    sectCfg[0].sectionConfig.policy.authPlainAccess         = 0;
    sectCfg[0].sectionConfig.policy.checksumIntegrity       = 0;
    sectCfg[0].sectionConfig.policy.digestIntegrity         = 0;
    sectCfg[0].sectionConfig.policy.digestIntegrityOnAccess = 0;
    sectCfg[0].sectionConfig.policy.rollbackProt            = 0;
    sectCfg[0].sectionConfig.policy.slog                    = 0;
    sectCfg[0].sectionConfig.policy.writeProt               = 0;

    /* --- Section 1: e --- */
    IMSG("Configuring section for storage %u\n", W77Q_STORAGE_SECTION);
    sectCfg[W77Q_STORAGE_SECTION].baseAddr = W77Q_SECTION0_SIZE_MB * 1024U * 1024U;
    sectCfg[W77Q_STORAGE_SECTION].size     = W77Q_SECTION1_SIZE_MB * 1024U * 1024U;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.plainAccessReadEnable   = 0;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.plainAccessWriteEnable  = 0;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.authPlainAccess         = 0;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.checksumIntegrity       = 0;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.digestIntegrity         = 0;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.digestIntegrityOnAccess = 0;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.rollbackProt            = 0;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.slog                    = 0;
    sectCfg[W77Q_STORAGE_SECTION].sectionConfig.policy.writeProt               = 0;
    res = derive_section_key_L((const uint8_t *)id.sec.wid, "W77Q_SECTION1_FK", (uint8_t *)fk);
    if (res)
    {
        EMSG("w77q_qlib: FK derivation failed: %#" PRIx32, res);
        return TEE_ERROR_GENERIC;
    }
    res = derive_section_key_L((const uint8_t *)id.sec.wid, "W77Q_SECTION1_RK", (uint8_t *)rk);
    if (res)
    {
        EMSG("w77q_qlib: RK derivation failed: %#" PRIx32, res);
        return TEE_ERROR_GENERIC;
    }

    memcpy(sectCfg[W77Q_STORAGE_SECTION].fullAccessKey, fk, sizeof(KEY_T));
    memcpy(sectCfg[W77Q_STORAGE_SECTION].restrictedKey, rk, sizeof(KEY_T));

    flashCfg.stdAddrSize.addrLen_u8 = QLIB_STD_ADDR_LEN_DEFAULT;

    /* Chip may report fastReadDummyCycles=31 (unprovisioned/OTP default) which
     * exceeds SPI_EXTCFG_MAX_DUMMY=30 and causes QLIB_ConfigDeviceMultiDie to
     * return INVALID_PARAMETER.  Clamp to the reset default (8) when out of range. */
    if (flashCfg.fastReadDummyCycles == 0 || flashCfg.fastReadDummyCycles > SPI_EXTCFG_MAX_DUMMY)
    {
        IMSG("w77q_qlib: clamping fastReadDummyCycles %u -> %u (FLASH_CR_DUMMY_DEFAULT)",
             flashCfg.fastReadDummyCycles, FLASH_CR_DUMMY_DEFAULT);
        flashCfg.fastReadDummyCycles = FLASH_CR_DUMMY_DEFAULT;
    }

    /*-------------------------------------------------------------------------------------------------------
     Step 5: Write configuration to flash
    -------------------------------------------------------------------------------------------------------*/
    IMSG("w77q_qlib: QLIB_ConfigDeviceMultiDie ...");
    status = QLIB_ConfigDeviceMultiDie(&g_qlib_ctx, &flashCfg, &dieCfg, cfgLine, diesNum_u8);
    if (status != QLIB_STATUS__OK)
    {
        EMSG("Error: QLIB_ConfigDeviceMultiDie failed (0x%X)\n", status);
        return TEE_ERROR_GENERIC;
    }
    IMSG("w77q_qlib: QLIB_ConfigDeviceMultiDie OK");

    return res;
}

TEE_Result w77q_init(struct w77q *dev, struct spi_chip *spi __unused)
{
    QLIB_STATUS_T st = QLIB_STATUS__OK;
    TEE_Result res = TEE_SUCCESS;
    QLIB_ID_T id = {0};
    KEY_T fk = {0};
    KEY_T rk = {0};
    uint32_t s1_base_u32 = 0, s1_size = 0;
    QLIB_SECTION_CONF_T s1_conf = {0};
    static const uint32_t zero_wid_u32[2] = {0};

    IMSG("w77q_qlib: >>> w77q_init enter");

    IMSG("w77q_qlib: QLIB_InitLib ...");
    st = QLIB_InitLib(&g_qlib_ctx);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_InitLib failed: %u", st);
        return TEE_ERROR_GENERIC;
    }
    IMSG("w77q_qlib: QLIB_InitLib OK");

    IMSG("w77q_qlib: QLIB_Connect ...");
    st = QLIB_Connect(&g_qlib_ctx);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_Connect failed: %u", st);
        return TEE_ERROR_GENERIC;
    }
    IMSG("w77q_qlib: QLIB_Connect OK");

    /* ---- Perform MC maintenance before InitDevice (MC_MAINT blocks config cmds) ---- */
    IMSG("w77q_qlib: QLIB_PerformMaintenance ...");
    st = QLIB_PerformMaintenance(&g_qlib_ctx);
    if (st != QLIB_STATUS__OK)
        IMSG("w77q_qlib: QLIB_PerformMaintenance returned %u (non-fatal)", st);
    else
        IMSG("w77q_qlib: QLIB_PerformMaintenance OK");

    IMSG("w77q_qlib: QLIB_InitDevice ...");
    st = QLIB_InitDevice(&g_qlib_ctx, QLIB_BUS_FORMAT(QLIB_BUS_MODE_1_1_1, false));
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_InitDevice failed: %u - falling back to plain mode", st);
        goto plain_mode;
    }
    IMSG("w77q_qlib: QLIB_InitDevice OK");

    /* ---- Read hardware WID and derive section keys ------------------- */
    IMSG("w77q_qlib: QLIB_GetId ...");
    st = QLIB_GetId(&g_qlib_ctx, &id);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_GetId failed: %u - cannot derive keys", st);
        return TEE_ERROR_GENERIC;
    }

    if ((memcmp(id.sec.wid, zero_wid_u32, sizeof(zero_wid_u32)) == 0) ||
        (id.sec.wid[0] == 0xffffffff && id.sec.wid[1] == 0xffffffff))
        {
        EMSG("w77q_qlib: WID is all-zeros - device not provisioned or "
             "hardware fault. WID should be provisioned in FAB.Security error");
        return TEE_ERROR_GENERIC;
    }

    /* WID is uint32_t[2] - print as two 32-bit hex words */
    IMSG("w77q_qlib: WID = 0x%08" PRIx32 " 0x%08" PRIx32, id.sec.wid[0], id.sec.wid[1]);

    /* Derive section keys from WID (needed for both provisioning and normal boot). */
    IMSG("w77q_qlib: derive FK ...");
    res = derive_section_key_L((const uint8_t *)id.sec.wid, "W77Q_SECTION1_FK", (uint8_t *)fk);
    if (res)
    {
        EMSG("w77q_qlib: FK derivation failed: %#" PRIx32, res);
        goto plain_mode;
    }
    IMSG("w77q_qlib: derive RK ...");
    res = derive_section_key_L((const uint8_t *)id.sec.wid, "W77Q_SECTION1_RK", (uint8_t *)rk);
    if (res)
    {
        EMSG("w77q_qlib: RK derivation failed: %#" PRIx32, res);
        goto plain_mode;
    }
    IMSG("w77q_qlib: key derivation OK");

    /*
     * Check section 1 provisioning status BEFORE attempting configChip.
     * QLIB_ConfigDeviceMultiDie() is a slow flash-write operation - running
     * it unconditionally on every boot causes the startup stall.
     * Only provision if section 1 is not yet sized (first boot / fresh chip).
     */
    IMSG("w77q_qlib: QLIB_GetDeviceSectionConfig(1) ...");
    st = QLIB_GetDeviceSectionConfig(&g_qlib_ctx, W77Q_STORAGE_SECTION, &s1_base_u32, &s1_size,
                                     &s1_conf);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: GetDeviceSectionConfig(1) failed: %u", st);
        goto plain_mode;
    }
    IMSG("w77q_qlib: section 1 config: base=0x%08" PRIx32 " size=0x%08" PRIx32,
         s1_base_u32, s1_size);

    if (s1_size != (uint32_t)W77Q_SECTION1_SIZE_MB * 1024U * 1024U)
    {
        /*
         * Section not provisioned (size=0) OR provisioned to a different size
         * than the compiled target - run configuration to (re)size it.
         * This makes a change to W77Q_CFG_SECTION1_SIZE_MB take effect on the
         * next boot without a manual chip wipe.
         */
        IMSG("w77q_qlib: section 1 size=0x%08" PRIx32 " != target %u MB - running w77q_configChip ...",
             s1_size, W77Q_SECTION1_SIZE_MB);
        res = w77q_configChip();
        if (res != TEE_SUCCESS)
        {
            EMSG("w77q_qlib: w77q_configChip failed: %#" PRIx32, res);
            goto plain_mode;
        }
        IMSG("w77q_qlib: w77q_configChip OK");
    }
    else
    {
        IMSG("w77q_qlib: section 1 already sized to target - skipping configChip");
    }

    /* ---- Load keys and open authenticated session on section 1 ------- */
    IMSG("w77q_qlib: QLIB_LoadKey(FK) ...");
    st = QLIB_LoadKey(&g_qlib_ctx, W77Q_STORAGE_SECTION, fk, true /* fullAccess */);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_LoadKey(FK) failed: %u", st);
        goto try_provision;
    }
    IMSG("w77q_qlib: QLIB_LoadKey(FK) OK");

    IMSG("w77q_qlib: QLIB_LoadKey(RK) ...");
    st = QLIB_LoadKey(&g_qlib_ctx, W77Q_STORAGE_SECTION, rk, false /* restricted */);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_LoadKey(RK) failed: %u", st);
        goto try_provision;
    }
    IMSG("w77q_qlib: QLIB_LoadKey(RK) OK");

    IMSG("w77q_qlib: QLIB_OpenSession(section 1) ...");
    st = QLIB_OpenSession(&g_qlib_ctx, W77Q_STORAGE_SECTION, QLIB_SESSION_ACCESS_FULL);
    if (st == QLIB_STATUS__OK)
    {
        /* Re-read section config for accurate size after possible configChip. */
        QLIB_GetDeviceSectionConfig(&g_qlib_ctx, W77Q_STORAGE_SECTION,
                                    &s1_base_u32, &s1_size, &s1_conf);
        g_sec1_ready_b_L = true;
        dev->size = (size_t)s1_size;
        IMSG("w77q_qlib: section 1 authenticated session OPEN - "
             "hardware integrity protection active (size=%" PRIu32 " MB)",
             s1_size / (1024U * 1024U));
        return TEE_SUCCESS;
    }
    EMSG("w77q_qlib: QLIB_OpenSession(section 1) failed: %u", st);

try_provision:
    /*
     * OpenSession failed - chip may have geometry from WFORMAT/FAB defaults
     * but no keys. Try configChip to set layout + keys, then retry.
     */
    IMSG("w77q_qlib: session failed - attempting configChip to provision keys ...");
    res = w77q_configChip();
    if (res != TEE_SUCCESS)
    {
        EMSG("w77q_qlib: w77q_configChip failed: %#" PRIx32 " - plain mode", res);
        goto plain_mode;
    }
    IMSG("w77q_qlib: w77q_configChip OK - retrying session ...");

    /* Reload keys and retry session after provisioning. */
    st = QLIB_LoadKey(&g_qlib_ctx, W77Q_STORAGE_SECTION, fk, true);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_LoadKey(FK) retry failed: %u", st);
        goto plain_mode;
    }
    st = QLIB_LoadKey(&g_qlib_ctx, W77Q_STORAGE_SECTION, rk, false);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_LoadKey(RK) retry failed: %u", st);
        goto plain_mode;
    }
    st = QLIB_OpenSession(&g_qlib_ctx, W77Q_STORAGE_SECTION, QLIB_SESSION_ACCESS_FULL);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_OpenSession retry failed: %u - plain mode", st);
        goto plain_mode;
    }

    /* Re-read section config for accurate size. */
    QLIB_GetDeviceSectionConfig(&g_qlib_ctx, W77Q_STORAGE_SECTION,
                                &s1_base_u32, &s1_size, &s1_conf);
    g_sec1_ready_b_L = true;
    dev->size = (size_t)s1_size;
    IMSG("w77q_qlib: section 1 authenticated session OPEN after provision - "
         "hardware integrity protection active (size=%" PRIu32 " MB)",
         s1_size / (1024U * 1024U));
    return TEE_SUCCESS;

plain_mode:
    g_sec1_ready_b_L = false;
    //dev->size = (size_t)(W77Q_SECTION0_SIZE_MB + W77Q_SECTION1_SIZE_MB) * 1024U * 1024U;
    // For chip without provisioning, the section1 size is 16MB as configured in FAB
    dev->size = (size_t)16u * 1024U * 1024U;
    IMSG("w77q_qlib: operating in PLAIN MODE - no HW-level integrity. "
         "NOT suitable for production.");
    return TEE_SUCCESS;
}

/*
 * w77q_read() - Read @len bytes at section-1-relative @addr_u32.
 * When the authenticated session is open, QLIB verifies hardware MAC.
 */
TEE_Result w77q_read(struct w77q *dev __unused, uint32_t addr_u32, void *buf_pv, size_t len)
{
    if (len > UINT32_MAX)
        return TEE_ERROR_BAD_PARAMETERS;

#ifdef W77Q_CFG_DEBUG_IO
    IMSG("[w77q_dbg] [READ] sec=%u addr=0x%08" PRIx32 " len=%zu secure=%d",
         W77Q_STORAGE_SECTION, addr_u32, len, g_sec1_ready_b_L);
#endif
    QLIB_STATUS_T st = QLIB_Read(&g_qlib_ctx, (uint8_t *)buf_pv, W77Q_STORAGE_SECTION, addr_u32,
                                 (uint32_t)len, g_sec1_ready_b_L /* secure */, false /* auth */);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: Read sec1+0x%08" PRIx32 " len %zu: err=%u", addr_u32, len, st);
        return TEE_ERROR_GENERIC;
    }
#ifdef W77Q_CFG_DEBUG_IO
    IMSG("[w77q_dbg] [READ OK] addr=0x%08" PRIx32 " len=%zu", addr_u32, len);
#endif
    return TEE_SUCCESS;
}

/*
 * w77q_read_plain() - Attempt to read section 1 WITHOUT the authenticated session.
 *
 * Forces secure=false regardless of whether a session is open.
 * When the section policy has plainAccessReadEnable=0, the W77Q hardware
 * will reject this read - demonstrating that data cannot be accessed
 * without proper authentication.
 */
TEE_Result w77q_read_plain(struct w77q *dev __unused, uint32_t addr_u32, void *buf_pv, size_t len)
{
    if (len > UINT32_MAX)
        return TEE_ERROR_BAD_PARAMETERS;

    QLIB_STATUS_T st = QLIB_Read(&g_qlib_ctx, (uint8_t *)buf_pv, W77Q_STORAGE_SECTION, addr_u32,
                                 (uint32_t)len, false /* secure=false: no session */,
                                 false /* auth */);
    if (st != QLIB_STATUS__OK)
    {
        IMSG("w77q_qlib: Plain read sec1+0x%08" PRIx32 " len %zu REJECTED: err=%u "
             "(this is expected - hardware enforces authenticated access)",
             addr_u32, len, st);
        return TEE_ERROR_ACCESS_DENIED;
    }
    IMSG("w77q_qlib: Plain read sec1+0x%08" PRIx32 " len %zu SUCCEEDED "
         "(section allows plain read - consider tightening policy)", addr_u32, len);
    return TEE_SUCCESS;
}

/*
 * w77q_write() - Write @len bytes at section-1-relative @addr_u32.
 * The target region must have been erased prior to this call.
 */
TEE_Result w77q_write(struct w77q *dev __unused, uint32_t addr_u32, const void *buf_pv, size_t len)
{
    if (len > UINT32_MAX)
        return TEE_ERROR_BAD_PARAMETERS;

#ifdef W77Q_CFG_DEBUG_IO
    IMSG("[w77q_dbg] [WRITE] sec=%u addr=0x%08" PRIx32 " len=%zu secure=%d",
         W77Q_STORAGE_SECTION, addr_u32, len, g_sec1_ready_b_L);
#endif
    QLIB_STATUS_T st = QLIB_Write(&g_qlib_ctx, (const uint8_t *)buf_pv, W77Q_STORAGE_SECTION, addr_u32,
                                  (uint32_t)len, g_sec1_ready_b_L /* secure */);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: Write sec1+0x%08" PRIx32 " len %zu: err=%u", addr_u32, len, st);
        return TEE_ERROR_GENERIC;
    }
#ifdef W77Q_CFG_DEBUG_IO
    IMSG("[w77q_dbg] [WRITE OK] addr=0x%08" PRIx32 " len=%zu", addr_u32, len);
#endif
    return TEE_SUCCESS;
}

/*
 * w77q_erase_sector() - Erase the 4 KB sector at section-1-relative @addr_u32.
 * @addr_u32 must be 4 KB aligned.
 */
TEE_Result w77q_erase_sector(struct w77q *dev __unused, uint32_t addr_u32)
{
#ifdef W77Q_CFG_DEBUG_IO
    IMSG("[w77q_dbg] [ERASE] sec=%u addr=0x%08" PRIx32 " size=0x%x secure=%d",
         W77Q_STORAGE_SECTION, addr_u32 & ~(W77Q_SECTOR_SIZE - 1U),
         W77Q_SECTOR_SIZE, g_sec1_ready_b_L);
#endif
    QLIB_STATUS_T st =
        QLIB_Erase(&g_qlib_ctx, W77Q_STORAGE_SECTION, addr_u32 & ~(W77Q_SECTOR_SIZE - 1U),
                   W77Q_SECTOR_SIZE, g_sec1_ready_b_L /* secure */);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: Erase sec1+0x%08" PRIx32 ": err=%u", addr_u32, st);
        return TEE_ERROR_GENERIC;
    }
#ifdef W77Q_CFG_DEBUG_IO
    IMSG("[w77q_dbg] [ERASE OK] addr=0x%08" PRIx32, addr_u32 & ~(W77Q_SECTOR_SIZE - 1U));
#endif
    return TEE_SUCCESS;
}
