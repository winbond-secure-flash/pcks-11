/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_config.h
* @brief      User-editable configuration for W77Q/T keys and flash sections
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
// SPDX-License-Identifier: BSD-2-Clause

#ifndef W77Q_CONFIG_H__
#define W77Q_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

/**********************************************************************************************************
                                                DEFINITIONS
**********************************************************************************************************/

/*---------------------------------------------------------------------------------------------------------
 * Cryptographic Keys
 *
 * WARNING: W77Q_CFG_KD is a SAMPLE/DEVELOPMENT value with NO security.
 *          You MUST replace it with your own unique 128-bit key before production use.
 *          W77Q_CFG_KD1 must match the factory key burned in the chip — do NOT change it.
 *
 * Each key is an array of 4 x uint32_t (128 bits total).
 *---------------------------------------------------------------------------------------------------------*/

/**
 * @brief  Device master key (KD) — customer-chosen key used to authenticate the host
 *         to the W77Q/T device. MUST be replaced with a unique production key.
 */
#define W77Q_CFG_KD                                \
{                                                  \
    0x11111111, 0x12121212, 0x13131313, 0x14141414 \
}

/**
 * @brief  Pre-provisioning master key (KD1) — the factory key burned in the chip by Winbond.
 *         DO NOT CHANGE — this value must match what is in the chip's OTP.
 *         Used only during first-time provisioning to authenticate before writing KD.
 */
#define W77Q_CFG_KD1                               \
{                                                  \
    0x01010101, 0x02020202, 0x03030303, 0x04040404 \
}

/*---------------------------------------------------------------------------------------------------------
 * Flash Section Configuration
 *
 * The W77Q/T flash is divided into sections. Each section has a size and access policy.
 * Section 0 is typically used for boot firmware (read-only after provisioning).
 * Section 1 is used for PKCS#11 secure object storage (read/write, authenticated).
 *---------------------------------------------------------------------------------------------------------*/

/**
 * @brief  Section 0 size in megabytes (boot firmware area).
 */
#define W77Q_CFG_SECTION0_SIZE_MB   4U

/**
 * @brief  Section 1 size in megabytes (PKCS#11 storage area).
 *         Increase this value to use more of the W77Q/T flash for key/object storage.
 *         Maximum is 16 MB per section (hardware limit); larger capacity needs
 *         multiple sections and/or the second die (die 0/1).
 */
#define W77Q_CFG_SECTION1_SIZE_MB   16U

/**
 * @brief  Which section is used for OP-TEE secure storage (PKCS#11 objects).
 */
#define W77Q_CFG_STORAGE_SECTION    1U

/*---------------------------------------------------------------------------------------------------------
 * Debug Configuration
 *---------------------------------------------------------------------------------------------------------*/

/**
 * @brief  Enable verbose flash I/O debug prints.
 *         When defined, every read, write, and erase operation on the W77Q/T flash
 *         prints address, size, mode (secure/plain), and result to the OP-TEE console.
 *         Useful for debugging the PKCS#11 ↔ flash data path.
 *
 *         Comment out or remove this line to disable debug output.
 */
/* #define W77Q_CFG_DEBUG_IO */

#ifdef __cplusplus
}
#endif

#endif /* W77Q_CONFIG_H__ */
