/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       main.c
* @brief      Secure Counter Client Application for Normal World
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: MIT */

/*-----------------------------------------------------------------------------------------------------------
                                                INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include <err.h>
#include <stdio.h>
#include <string.h>

#include <tee_client_api.h>

/* shared TA/CA header — defines TA_SECURE_COUNTER_UUID and CMD_* */
#include "secure_counter_ta.h"

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/
#define INCREMENT_COUNT 3u
#define ADD_VALUE       10u

/* ── helper: invoke a command and return the uint32 result value ──────── */

static uint32_t invoke_get_value_L(TEEC_Session *sess, uint32_t cmd_u32)
{
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT,
                                     TEEC_NONE, TEEC_NONE, TEEC_NONE);

    TEEC_Result res = TEEC_InvokeCommand(sess, cmd_u32, &op, &err_origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_InvokeCommand(cmd_u32=%u) failed: 0x%x (origin 0x%x)",
             cmd_u32, res, err_origin_u32);

    return op.params[0].value.a;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    TEEC_Result     res;
    TEEC_Context    ctx;
    TEEC_Session    sess;
    TEEC_Operation  op;
    TEEC_UUID       uuid = TA_SECURE_COUNTER_UUID;
    uint32_t        err_origin_u32;

    printf("═══════════════════════════════════════\n");
    printf(" Secure Counter — OP-TEE demo\n");
    printf("═══════════════════════════════════════\n\n");

    /* 1. Connect to TEE */
    res = TEEC_InitializeContext(NULL, &ctx);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_InitializeContext failed: 0x%x", res);
    printf("[OK] TEE context initialised\n");

    /* 2. Open session with the Secure Counter TA */
    res = TEEC_OpenSession(&ctx, &sess, &uuid,
                           TEEC_LOGIN_PUBLIC, NULL, NULL, &err_origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_OpenSession failed: 0x%x (origin 0x%x)",
             res, err_origin_u32);
    printf("[OK] Session opened with TA  6e256cba-fc4d-4c2b-a7f2-6b5c4d9e7a8b\n\n");

    /* 3. Get initial value */
    uint32_t val_u32 = invoke_get_value_L(&sess, CMD_GET);
    printf("CMD_GET       → counter = %u\n", val_u32);

    /* 4. Increment three times */
    for (int32_t i_i32 = 0; i_i32 < (int32_t)INCREMENT_COUNT; i_i32++)
    {
        val_u32 = invoke_get_value_L(&sess, CMD_INCREMENT);
        printf("CMD_INCREMENT → counter = %u\n", val_u32);
    }

    /* 5. Add 10 */
    memset(&op, 0, sizeof(op));
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INOUT,
                                     TEEC_NONE, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = ADD_VALUE;   /* addend */

    res = TEEC_InvokeCommand(&sess, CMD_ADD, &op, &err_origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "CMD_ADD failed: 0x%x", res);
    printf("CMD_ADD(%u)   → counter = %u\n", ADD_VALUE, op.params[0].value.b);

    /* 6. Get current value */
    val_u32 = invoke_get_value_L(&sess, CMD_GET);
    printf("CMD_GET       → counter = %u\n", val_u32);

    /* 7. Reset */
    memset(&op, 0, sizeof(op));
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE, TEEC_NONE, TEEC_NONE);
    res = TEEC_InvokeCommand(&sess, CMD_RESET, &op, &err_origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "CMD_RESET failed: 0x%x", res);
    printf("CMD_RESET     → done\n");

    /* 8. Confirm reset */
    val_u32 = invoke_get_value_L(&sess, CMD_GET);
    printf("CMD_GET       → counter = %u\n", val_u32);

    /* 9. Close session + context */
    TEEC_CloseSession(&sess);
    TEEC_FinalizeContext(&ctx);

    printf("\n[OK] Session closed — all done\n");
    return 0u;
}
