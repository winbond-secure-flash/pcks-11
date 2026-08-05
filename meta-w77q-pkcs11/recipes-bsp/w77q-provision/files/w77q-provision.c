/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q-provision.c
* @brief      Userspace tool to provision a W77Q flash section key
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/
/* SPDX-License-Identifier: BSD-2-Clause */

/*-----------------------------------------------------------------------------------------------------------
                                                INCLUDES
-----------------------------------------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <tee_client_api.h>

/*-----------------------------------------------------------------------------------------------------------
                                                DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/
/* UUID and commands — keep in sync with core/include/w77q_provision_ta.h */
#define W77Q_PROVISION_TA_UUID \
    { 0x7d2b4a3fU, 0x8c5eU, 0x4f2aU, \
      { 0x9bU, 0x1dU, 0x6eU, 0x8fU, 0x3cU, 0x7aU, 0x2bU, 0x5dU } }

#define W77Q_PROVISION_CMD_SET_KEY  0U
#define W77Q_PROVISION_KEY_SIZE     16U
#define EXPECTED_ARG_COUNT          3

static int32_t hex_to_bytes_L(const char *hex, uint8_t *out_pu8, size_t len)
{
    size_t i = 0;
    size_t hexlen = strlen(hex);

    if (hexlen != len * 2u)
    {
        fprintf(stderr, "Error: expected %zu hex digits, got %zu\n",
            len * 2u, hexlen);
        return -1u;
    }

    for (i = 0; i < len; i++)
    {
        uint32_t byte_u32 = 0;

        if (sscanf(hex + i * 2u, "%02x", &byte_u32) != 1)
        {
            fprintf(stderr, "Error: invalid hex at position %zu\n",
                i * 2u);
            return -1u;
        }
        out_pu8[i] = (uint8_t)byte_u32;
    }
    return 0u;
}

static void print_key_L(const char *label, const uint8_t *key_pu8, size_t len)
{
    size_t i = 0;

    printf("%s: ", label);
    for (i = 0; i < len; i++)
        printf("%02X", key_pu8[i]);
    printf("\n");
}

int main(int32_t argc_i32, char *argv[])
{
    TEEC_Result      res        = TEEC_SUCCESS;
    TEEC_Context     ctx        = { };
    TEEC_Session     session    = { };
    TEEC_Operation   op         = { };
    TEEC_UUID        uuid       = W77Q_PROVISION_TA_UUID;
    uint32_t         ret_origin_u32 = 0;
    uint8_t          master_key_u8[W77Q_PROVISION_KEY_SIZE] = { };
    uint8_t          section_key_u8[W77Q_PROVISION_KEY_SIZE] = { };

    if (argc_i32 != EXPECTED_ARG_COUNT)
    {
        fprintf(stderr,
            "Usage: %s <masterKey-hex32> <sectionKey-hex32>\n"
            "\n"
            "  masterKey   32 hex digits (00..0 for factory-fresh)\n"
            "  sectionKey  32 hex digits (key_pu8 to burn into W77Q)\n"
            "\n"
            "  Example:\n"
            "    %s 00000000000000000000000000000000"
            " DEADBEEFCAFEBABE1234567890ABCDEF\n",
            argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    if (hex_to_bytes_L(argv[1], master_key_u8, W77Q_PROVISION_KEY_SIZE) ||
        hex_to_bytes_L(argv[2], section_key_u8, W77Q_PROVISION_KEY_SIZE))
        {
        return EXIT_FAILURE;
    }

    printf("Master key:  [%u bytes loaded]\n", W77Q_PROVISION_KEY_SIZE);
    printf("Section key: [%u bytes loaded]\n", W77Q_PROVISION_KEY_SIZE);

    /* Initialize TEE context */
    res = TEEC_InitializeContext(NULL, &ctx);
    if (res != TEEC_SUCCESS)
    {
        fprintf(stderr, "TEEC_InitializeContext failed: 0x%08x\n", res);
        return EXIT_FAILURE;
    }

    /* Open session with the W77Q provisioning Pseudo-TA */
    res = TEEC_OpenSession(&ctx, &session, &uuid,
                   TEEC_LOGIN_PUBLIC, NULL, NULL, &ret_origin_u32);
    if (res != TEEC_SUCCESS)
    {
        fprintf(stderr,
            "TEEC_OpenSession failed: 0x%08x (origin 0x%08x)\n",
            res, ret_origin_u32);
        fprintf(stderr, "Is tee-supplicant running? "
            "Is CFG_W77Q_QLIB_PROVISION=y in OP-TEE build?\n");
        TEEC_FinalizeContext(&ctx);
        return EXIT_FAILURE;
    }

    /* Invoke SET_KEY with master key and section key as MEMREF params */
    memset(&op, 0, sizeof(op));
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
                     TEEC_MEMREF_TEMP_INPUT,
                     TEEC_NONE,
                     TEEC_NONE);
    op.params[0].tmpref.buffer = master_key_u8;
    op.params[0].tmpref.size   = W77Q_PROVISION_KEY_SIZE;
    op.params[1].tmpref.buffer = section_key_u8;
    op.params[1].tmpref.size   = W77Q_PROVISION_KEY_SIZE;

    res = TEEC_InvokeCommand(&session, W77Q_PROVISION_CMD_SET_KEY,
                 &op, &ret_origin_u32);
    if (res != TEEC_SUCCESS)
    {
        fprintf(stderr,
            "W77Q provisioning FAILED: 0x%08x (origin 0x%08x)\n",
            res, ret_origin_u32);
        fprintf(stderr,
            "Possible causes:\n"
            "  - Wrong masterKey (device already provisioned?)\n"
            "  - W77Q not connected or QLIB_Connect failed\n"
            "  - Section already provisioned with different key_pu8\n"
            "    (use QLIB_Format to erase and re-provision)\n");
        TEEC_CloseSession(&session);
        TEEC_FinalizeContext(&ctx);
        return EXIT_FAILURE;
    }

    printf("W77Q section key_pu8 provisioned successfully.\n");
    printf("IMPORTANT: Record the section key_pu8 in your key_pu8 management system.\n");
    printf("Set CFG_W77Q_QLIB_SECTION_KEY in the OP-TEE firmware build.\n");

    TEEC_CloseSession(&session);
    TEEC_FinalizeContext(&ctx);

    /* Zero keys from stack before exit */
    explicit_bzero(master_key_u8,  sizeof(master_key_u8));
    explicit_bzero(section_key_u8, sizeof(section_key_u8));

    return EXIT_SUCCESS;
}
