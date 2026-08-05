/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_provision_ta.h
* @brief      Shared UUID and command definitions for the W77Q provisioning Pseudo-TA
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef W77Q_PROVISION_TA_H__
#define W77Q_PROVISION_TA_H__

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/*
 * W77Q provisioning Pseudo-TA UUID:
 *   7d2b4a3f-8c5e-4f2a-9b1d-6e8f3c7a2b5d
 */
#define W77Q_PROVISION_TA_UUID \
    { 0x7d2b4a3fU, 0x8c5eU, 0x4f2aU, \
      { 0x9bU, 0x1dU, 0x6eU, 0x8fU, 0x3cU, 0x7aU, 0x2bU, 0x5dU } }

/*
 * W77Q_PROVISION_CMD_SET_KEY — Program the section key into W77Q hardware.
 *
 * params[0]: MEMREF_INPUT  masterKey   (16 bytes) — current Device Master Key
 *                                       (all-zero on factory-fresh chip)
 * params[1]: MEMREF_INPUT  sectionKey  (16 bytes) — new 128-bit full-access
 *                                       key to burn into the W77Q section
 *
 * Returns TEE_SUCCESS on success, TEE_ERROR_GENERIC on QLIB error,
 * TEE_ERROR_BAD_PARAMETERS if buffer sizes are wrong.
 */
#define W77Q_PROVISION_CMD_SET_KEY  0U

/* Key size in bytes (KEY_T = uint32_t[4] = 16 bytes) */
#define W77Q_PROVISION_KEY_SIZE     16U

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // W77Q_PROVISION_TA_H__
