/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       secure_counter_ta.c
* @brief      Secure Counter Trusted Application for OP-TEE Secure World
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: MIT */

/*-----------------------------------------------------------------------------------------------------------
                                                INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>

#include "secure_counter_ta.h"

/*-----------------------------------------------------------------------------------------------------------
                                             LOCAL VARIABLES
-----------------------------------------------------------------------------------------------------------*/

/* TEE-side counter (volatile — not shared between sessions) ───────────
 * Declared static so each TA instance has its own counter.
 * For a shared counter across sessions use a persistent object.      */
static uint32_t g_counter_u32_L = 0;

/* ── Lifecycle entry points ─────────────────────────────────────────────── */

TEE_Result TA_CreateEntryPoint(void)
{
    DMSG("secure_counter: TA_CreateEntryPoint");
    g_counter_u32_L = 0;
    return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
    DMSG("secure_counter: TA_DestroyEntryPoint");
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types_u32 __unused,
                                    TEE_Param params[4] __unused,
                                    void **sess_ctx_pv __unused)
{
    DMSG("secure_counter: session opened");
    return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *sess_ctx_pv __unused)
{
    DMSG("secure_counter: session closed");
}

/* ── Command dispatcher ──────────────────────────────────────────────────── */

TEE_Result TA_InvokeCommandEntryPoint(void *sess_ctx_pv __unused,
                                      uint32_t cmd_id_u32,
                                      uint32_t param_types_u32,
                                      TEE_Param params[4])
{
    switch (cmd_id_u32)
    {

    case CMD_INCREMENT:
        if (TEE_PARAM_TYPE_GET(param_types_u32, 0u) != TEE_PARAM_TYPE_VALUE_OUTPUT)
            return TEE_ERROR_BAD_PARAMETERS;
        g_counter_u32_L++;
        params[0].value.a = g_counter_u32_L;
        DMSG("secure_counter: increment → %u", g_counter_u32_L);
        return TEE_SUCCESS;

    case CMD_GET:
        if (TEE_PARAM_TYPE_GET(param_types_u32, 0u) != TEE_PARAM_TYPE_VALUE_OUTPUT)
            return TEE_ERROR_BAD_PARAMETERS;
        params[0].value.a = g_counter_u32_L;
        DMSG("secure_counter: get → %u", g_counter_u32_L);
        return TEE_SUCCESS;

    case CMD_RESET:
        g_counter_u32_L = 0;
        DMSG("secure_counter: reset");
        return TEE_SUCCESS;

    case CMD_ADD: {
        if (TEE_PARAM_TYPE_GET(param_types_u32, 0u) != TEE_PARAM_TYPE_VALUE_INOUT)
            return TEE_ERROR_BAD_PARAMETERS;
        uint32_t addend_u32 = params[0].value.a;
        g_counter_u32_L += addend_u32;
        params[0].value.b = g_counter_u32_L;
        DMSG("secure_counter: add %u → %u", addend_u32, g_counter_u32_L);
        return TEE_SUCCESS;
    }

    default:
        EMSG("secure_counter: unknown command 0x%x", cmd_id_u32);
        return TEE_ERROR_NOT_SUPPORTED;
    }
}
