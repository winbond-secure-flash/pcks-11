/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_provision_pta.c
* @brief      Pseudo-TA for W77Q section key provisioning
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
// SPDX-License-Identifier: BSD-2-Clause

/*-----------------------------------------------------------------------------------------------------------
                                                INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include <kernel/pseudo_ta.h>
#include <tee/tee_fs.h>
#include <trace.h>
#include <string.h>

#include "w77q_provision_ta.h"

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

/* Declared in core/drivers/spi/qlib/w77q_qlib_provision.c */
TEE_Result w77q_qlib_provision(const uint32_t masterKey_u32[4],
                   const uint32_t sectionKey_u32[4]);

#define PTA_NAME "w77q_provision.pta"

static TEE_Result invoke_command_L(void *session_ctx_pv __unused,
                 uint32_t cmd_u32,
                 uint32_t ptypes_u32,
                 TEE_Param params[TEE_NUM_PARAMS])
{
    const uint32_t exp_ptypes_u32 =
        TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
                TEE_PARAM_TYPE_MEMREF_INPUT,
                TEE_PARAM_TYPE_NONE,
                TEE_PARAM_TYPE_NONE);

    if (cmd_u32 != W77Q_PROVISION_CMD_SET_KEY)
        return TEE_ERROR_NOT_IMPLEMENTED;

    if (ptypes_u32 != exp_ptypes_u32)
        return TEE_ERROR_BAD_PARAMETERS;

    if (params[0].memref.size != W77Q_PROVISION_KEY_SIZE ||
        params[1].memref.size != W77Q_PROVISION_KEY_SIZE)
        return TEE_ERROR_BAD_PARAMETERS;

    IMSG("w77q_provision: received SET_KEY request from Normal World");
    IMSG("w77q_provision: WARNING re-provisioning will destroy all existing data");

    return w77q_qlib_provision(
        (const uint32_t *)params[0].memref.buffer, /* masterKey  */
        (const uint32_t *)params[1].memref.buffer  /* sectionKey */
    );
}

pseudo_ta_register(.uuid = W77Q_PROVISION_TA_UUID,
           .name = PTA_NAME,
           .flags = PTA_DEFAULT_FLAGS,
           .invoke_command_entry_point = invoke_command_L);
