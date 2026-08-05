/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       secure_counter_ta.h
* @brief      Shared header for the secure counter TA and CA, defines UUID and command IDs
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: MIT */

#ifndef SECURE_COUNTER_TA_H__
#define SECURE_COUNTER_TA_H__

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/* -----------------------------------------------------------------------
 * TA UUID:  6e256cba-fc4d-4c2b-a7f2-6b5c4d9e7a8b
 *
 * Generate your own for a real project:
 *   python3 -c "import uuid; print(uuid.uuid4())"
 * ----------------------------------------------------------------------- */
#define TA_SECURE_COUNTER_UUID \
    { 0x6e256cba, 0xfc4d, 0x4c2b, \
      { 0xa7, 0xf2, 0x6b, 0x5c, 0x4d, 0x9e, 0x7a, 0x8b } }

/*
 * CMD_INCREMENT — params[0]: VALUE_OUTPUT  → counter value after increment
 * CMD_GET       — params[0]: VALUE_OUTPUT  → current counter value
 * CMD_RESET     — no params
 * CMD_ADD       — params[0]: VALUE_INOUT   → .a = addend in, .b = result out
 */
#define CMD_INCREMENT   0
#define CMD_GET         1
#define CMD_RESET       2
#define CMD_ADD         3

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // SECURE_COUNTER_TA_H__
