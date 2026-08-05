/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_qlib_provision.c
* @brief      One-time provisioning of a QLIB password-protected section for OP-TEE secure storage
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: BSD-2-Clause */

/*-----------------------------------------------------------------------------------------------------------
                                                INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include <drivers/w77q.h>
#include <trace.h>
#include <tee_api_types.h>

#include "qlib.h"
#include "qlib_types.h"

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

/* Provided by w77q_qlib.c */
extern QLIB_CONTEXT_T  g_qlib_ctx;  /* already connected + InitDevice done */

#ifndef CFG_W77Q_QLIB_SECTION_ID
#define CFG_W77Q_QLIB_SECTION_ID  0U
#endif  // CFG_W77Q_QLIB_SECTION_ID

#ifndef CFG_W77Q_PART_OFFSET
#define CFG_W77Q_PART_OFFSET      0x200000U
#endif  // CFG_W77Q_PART_OFFSET

#ifndef CFG_W77Q_PART_SECTORS
#define CFG_W77Q_PART_SECTORS     128U
#endif  // CFG_W77Q_PART_SECTORS

/*
 * w77q_qlib_provision() - Burn section password into W77Q hardware.
 *
 * @masterKey:    Current Device Master Key (all-zero on factory-fresh chip).
 * @sectionKey:   128-bit full-access key to set for the OP-TEE storage section.
 *
 * On success the W77Q section CFG_W77Q_QLIB_SECTION_ID is configured with:
 *   - baseAddr = CFG_W77Q_PART_OFFSET
 *   - size     = CFG_W77Q_PART_SECTORS * W77Q_SECTOR_SIZE
 *   - policy:  authPlainAccess=1 (session required)
 *              plainAccessReadEnable=1  (LA reads allowed after session open)
 *              plainAccessWriteEnable=1 (LA writes allowed after session open)
 *   - fullAccessKey = sectionKey
 *
 * Call this ONCE at the factory.  w77q_init() will then call QLIB_LoadKey +
 * QLIB_OpenSession on every subsequent boot using CFG_W77Q_QLIB_SECTION_KEY.
 *
 * Returns TEE_SUCCESS or TEE_ERROR_GENERIC.
 */
TEE_Result w77q_qlib_provision(const KEY_T masterKey, const KEY_T sectionKey)
{
    QLIB_STATUS_T             st   = QLIB_STATUS__OK;
    QLIB_FLASH_CONFIG_T       flashCfg  = { };
    QLIB_DIE_CONFIG_T         dieCfg    = { };
    QLIB_SECTIONS_CONF_TABLE_T secTable = { };
    uint32_t i = 0;

    IMSG("w77q_qlib: provisioning section %u at 0x%08x size %u KB",
         (uint32_t)CFG_W77Q_QLIB_SECTION_ID,
         (uint32_t)CFG_W77Q_PART_OFFSET,
         (uint32_t)(CFG_W77Q_PART_SECTORS * W77Q_SECTOR_SIZE / 1024U));

    /*
     * flashCfg — global flash settings.
     * Zero-init is fine for defaults (watchdog off, single-address mode).
     */

    /*
     * dieCfg — set the current (existing) device master key so QLIB can
     * authenticate the provisioning operation.  Leave deviceMasterKey as
     * the same value if you do not want to change it; set a new value here
     * to rotate the DMK (requires the old key in masterKey to authorise).
     */
    for (i = 0; i < 4U; i++)
        dieCfg.deviceMasterKey[i] = masterKey[i];

    /*
     * Disable all sections by default (size=0), then enable only the
     * OP-TEE storage section.
     */
    for (i = 0; i < QLIB_NUM_OF_SECTIONS; i++)
        secTable.sectionConfigTable[i].size = 0; /* disabled */

    /* OP-TEE storage section */
    {
        QLIB_EXTENDED_SECTION_CONF_T *sec =
            &secTable.sectionConfigTable[CFG_W77Q_QLIB_SECTION_ID];

        sec->baseAddr = (uint32_t)CFG_W77Q_PART_OFFSET;
        sec->size     = (uint32_t)(CFG_W77Q_PART_SECTORS *
                       W77Q_SECTOR_SIZE);

        /* Policy: require session (authPlainAccess), allow LA R/W */
        sec->sectionConfig.policy.authPlainAccess        = 1;
        sec->sectionConfig.policy.plainAccessReadEnable  = 1;
        sec->sectionConfig.policy.plainAccessWriteEnable = 1;
        sec->sectionConfig.policy.checksumIntegrity      = 1;

        /* No plain access on power-up — session must be opened first */
        sec->resetPA = false;

        /* Full-access key (used by QLIB_LoadKey + QLIB_OpenSession) */
        for (i = 0; i < 4U; i++)
            sec->fullAccessKey[i] = sectionKey[i];

        /* Leave restrictedKey as zero (read-only access, not used) */
        /* Leave lmsKey as zero (LMS attestation not used) */
    }

    st = QLIB_ConfigDeviceMultiDie(&g_qlib_ctx,
                       &flashCfg,
                       &dieCfg,
                       &secTable,
                       1u /* numOfDies */);
    if (st != QLIB_STATUS__OK)
    {
        EMSG("w77q_qlib: QLIB_ConfigDeviceMultiDie failed: %u", st);
        return TEE_ERROR_GENERIC;
    }

    IMSG("w77q_qlib: section %u provisioned — password set",
         (uint32_t)CFG_W77Q_QLIB_SECTION_ID);
    return TEE_SUCCESS;
}
