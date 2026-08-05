/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       w77q_dump.c
* @brief      Normal World tool for the W77Q flash dump Pseudo-TA
*
* ### project meta-w77q-pkcs11
*
************************************************************************************************************/

/*-----------------------------------------------------------------------------------------------------------
                                                   INCLUDES
-----------------------------------------------------------------------------------------------------------*/

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <tee_client_api.h>

/* Shared header — defines UUID, commands, w77q_dump_entry */
#include "w77q_dump_ta.h"

/*-----------------------------------------------------------------------------------------------------------
                                                 DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

#define HEX_DUMP_COLS        (16u)
#define HEX_PREFIX_LEN       (2u)
#define MAX_TRANSFER_LEN     (65536u)
#define MIN_ARGC_CMD         (2u)
#define MIN_ARGC_ONE_PARAM   (3u)
#define MIN_ARGC_TWO_PARAMS  (4u)
#define MIN_ARGC_WRITE_FLAG  (5u)
#define HEX_CHARS_PER_BYTE   (2u)

/*-----------------------------------------------------------------------------------------------------------
                                               LOCAL VARIABLES
-----------------------------------------------------------------------------------------------------------*/

static TEEC_Context  g_ctx_L;
static TEEC_Session  g_sess_L;

/*-----------------------------------------------------------------------------------------------------------
                                          LOCAL FUNCTION PROTOTYPES
-----------------------------------------------------------------------------------------------------------*/

static void open_session_L(void);
static void close_session_L(void);
static void hex_dump_L(const uint8_t *buf_pu8, size_t len_u32);
static void uuid_str_L(const uint8_t u_u8[16], char out[37]);
static void cmd_list_all_L(void);
static void cmd_read_raw_L(uint32_t off_u32, uint32_t len_u32);
static void cmd_read_plain_L(uint32_t off_u32, uint32_t len_u32);
static void cmd_read_file_L(const char *ta_uuid_str, const char *obj_id_str);
static void cmd_write_raw_L(uint32_t off_u32, const uint8_t *data_pu8, uint32_t len_u32);
static void cmd_erase_sector_L(uint32_t off_u32);
static void cmd_erase_chip_L(void);
static void usage_L(void);

/*-----------------------------------------------------------------------------------------------------------
                                               LOCAL FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

static void open_session_L(void)
{
    TEEC_UUID uuid = W77Q_DUMP_TA_UUID;
    uint32_t origin_u32;

    TEEC_Result res = TEEC_InitializeContext(NULL, &g_ctx_L);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_InitializeContext: 0x%x", res);

    res = TEEC_OpenSession(&g_ctx_L, &g_sess_L, &uuid,
                           TEEC_LOGIN_PUBLIC, NULL, NULL, &origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_OpenSession: 0x%x (origin_u32 0x%x)", res, origin_u32);
}

static void close_session_L(void)
{
    TEEC_CloseSession(&g_sess_L);
    TEEC_FinalizeContext(&g_ctx_L);
}

static void hex_dump_L(const uint8_t *buf_pu8, size_t len_u32)
{
    for (size_t i = 0; i < len_u32; i += HEX_DUMP_COLS)
    {
        printf("%04zx  ", i);
        for (size_t j = i; j < i + HEX_DUMP_COLS; j++)
        {
            if (j < len_u32) printf("%02x ", buf_pu8[j]);
            else         printf("   ");
            if (j == i + 7u) printf(" ");
        }
        printf(" |");
        for (size_t j = i; j < i + HEX_DUMP_COLS && j < len_u32; j++)
            printf("%c", (buf_pu8[j] >= 0x20 && buf_pu8[j] < 0x7f) ? buf_pu8[j] : '.');
        printf("|\n");
    }
}

static void uuid_str_L(const uint8_t u_u8[16], char out[37])
{
    snprintf(out, 37u,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        u_u8[0],u_u8[1],u_u8[2],u_u8[3], u_u8[4],u_u8[5], u_u8[6],u_u8[7], u_u8[8],u_u8[9],
        u_u8[10],u_u8[11],u_u8[12],u_u8[13],u_u8[14],u_u8[15]);
}

/* ── list-all ──────────────────────────────────────────────────────────────── */
static void cmd_list_all_L(void)
{
    const size_t bufsz = W77Q_DUMP_MAX_ENTRIES * sizeof(struct w77q_dump_entry);
    struct w77q_dump_entry *entries = malloc(bufsz);
    if (entries == NULL) err(1u, "malloc");

    TEEC_Operation op = { 0 };
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE,
                                     TEEC_VALUE_OUTPUT,
                                     TEEC_NONE, TEEC_NONE);
    TEEC_SharedMemory shm = {
        .buffer = entries,
        .size   = bufsz,
        .flags  = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT,
    };
    TEEC_Result res = TEEC_RegisterSharedMemory(&g_ctx_L, &shm);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_RegisterSharedMemory: 0x%x", res);
    op.params[0].memref.parent = &shm;
    op.params[0].memref.offset = 0;
    op.params[0].memref.size   = bufsz;

    uint32_t origin_u32;
    res = TEEC_InvokeCommand(&g_sess_L, W77Q_DUMP_CMD_LIST_ALL, &op, &origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "LIST_ALL failed: 0x%x (origin_u32 0x%x)", res, origin_u32);

    uint32_t count_u32 = op.params[1].value.a;
    printf("[OK] W77Q flash LUT: %u_u8 object(s)\n\n", count_u32);

    for (uint32_t i = 0; i < count_u32; i++)
    {
        char uuid_s[37];
        char objid_hex[W77Q_DUMP_OBJ_ID_MAX * 2u + 1u];
        uint32_t olen_u32 = entries[i].obj_id_len_u32;

        if (olen_u32 > W77Q_DUMP_OBJ_ID_MAX)
            olen_u32 = W77Q_DUMP_OBJ_ID_MAX;
        for (uint32_t k = 0; k < olen_u32; k++)
            snprintf(objid_hex + k * 2u, 3u, "%02x", entries[i].obj_id[k]);
        objid_hex[olen_u32 * 2u] = '\0';

        uuid_str_L(entries[i].ta_uuid_u8, uuid_s);
        printf("[%03u] ta_uuid=%-36s  flash_off=0x%08x  data_size=%5u  obj_id=%s\n",
               i, uuid_s, entries[i].flash_off_u32,
               entries[i].data_size_u32, objid_hex);
    }

    TEEC_ReleaseSharedMemory(&shm);
    free(entries);
}

/* ── read-raw ──────────────────────────────────────────────────────────────── */
static void cmd_read_raw_L(uint32_t off_u32, uint32_t len_u32)
{
    uint8_t *buf_pu8 = malloc(len_u32);
    if (buf_pu8 == NULL) err(1u, "malloc");

    TEEC_SharedMemory shm = {
        .buffer = buf_pu8,
        .size   = len_u32,
        .flags  = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT,
    };
    TEEC_Result res = TEEC_RegisterSharedMemory(&g_ctx_L, &shm);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_RegisterSharedMemory: 0x%x", res);

    TEEC_Operation op = { 0 };
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
                                     TEEC_MEMREF_WHOLE,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].value.a         = off_u32;
    op.params[1].memref.parent   = &shm;
    op.params[1].memref.offset   = 0;
    op.params[1].memref.size     = len_u32;

    uint32_t origin_u32;
    res = TEEC_InvokeCommand(&g_sess_L, W77Q_DUMP_CMD_READ_RAW, &op, &origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "READ_RAW failed: 0x%x (origin_u32 0x%x)", res, origin_u32);

    printf("[OK] raw flash @ 0x%08x (%u_u8 bytes):\n", off_u32, len_u32);
    hex_dump_L(buf_pu8, len_u32);

    TEEC_ReleaseSharedMemory(&shm);
    free(buf_pu8);
}

/* ── read-plain ────────────────────────────────────────────────────────────── */
static void cmd_read_plain_L(uint32_t off_u32, uint32_t len_u32)
{
    uint8_t *buf_pu8 = malloc(len_u32);
    if (buf_pu8 == NULL) err(1u, "malloc");

    TEEC_SharedMemory shm = {
        .buffer = buf_pu8,
        .size   = len_u32,
        .flags  = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT,
    };
    TEEC_Result res = TEEC_RegisterSharedMemory(&g_ctx_L, &shm);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_RegisterSharedMemory: 0x%x", res);

    TEEC_Operation op = { 0 };
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
                                     TEEC_MEMREF_WHOLE,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].value.a         = off_u32;
    op.params[1].memref.parent   = &shm;
    op.params[1].memref.offset   = 0;
    op.params[1].memref.size     = len_u32;

    uint32_t origin_u32;
    res = TEEC_InvokeCommand(&g_sess_L, W77Q_DUMP_CMD_READ_PLAIN, &op, &origin_u32);
    if (res != TEEC_SUCCESS)
    {
        printf("[DENIED] Plain read @ 0x%08x (%u bytes) REJECTED by W77Q hardware\n"
               "         Error: 0x%x (origin 0x%x)\n"
               "         >> Data is protected — authenticated session required.\n",
               off_u32, len_u32, res, origin_u32);
        TEEC_ReleaseSharedMemory(&shm);
        free(buf_pu8);
        return;
    }

    printf("[WARNING] Plain read @ 0x%08x (%u bytes) SUCCEEDED — "
           "section allows unauthenticated access!\n", off_u32, len_u32);
    hex_dump_L(buf_pu8, len_u32);

    TEEC_ReleaseSharedMemory(&shm);
    free(buf_pu8);
}

/* ── write-raw ─────────────────────────────────────────────────────────────── */
static void cmd_write_raw_L(uint32_t off_u32, const uint8_t *data_pu8, uint32_t len_u32)
{
    TEEC_SharedMemory shm = {
        .buffer = (void *)data_pu8,
        .size   = len_u32,
        .flags  = TEEC_MEM_INPUT,
    };
    TEEC_Result res = TEEC_RegisterSharedMemory(&g_ctx_L, &shm);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_RegisterSharedMemory: 0x%x", res);

    TEEC_Operation op = { 0 };
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
                                     TEEC_MEMREF_WHOLE,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].value.a       = off_u32;
    op.params[1].memref.parent = &shm;
    op.params[1].memref.offset = 0;
    op.params[1].memref.size   = len_u32;

    uint32_t origin_u32;
    res = TEEC_InvokeCommand(&g_sess_L, W77Q_DUMP_CMD_WRITE_RAW, &op, &origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "WRITE_RAW failed: 0x%x (origin_u32 0x%x)", res, origin_u32);

    TEEC_ReleaseSharedMemory(&shm);
    printf("[OK] wrote %u_u8 bytes to flash @ 0x%08x\n", len_u32, off_u32);
}

/* ── erase-sector ──────────────────────────────────────────────────────────── */
static void cmd_erase_sector_L(uint32_t off_u32)
{
    TEEC_Operation op = { 0 };
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
                                     TEEC_NONE, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = off_u32;

    uint32_t origin_u32;
    TEEC_Result res = TEEC_InvokeCommand(&g_sess_L, W77Q_DUMP_CMD_ERASE_SECTOR,
                                         &op, &origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "ERASE_SECTOR failed: 0x%x (origin_u32 0x%x)", res, origin_u32);

    printf("[OK] sector @ 0x%08x erased (4 KB), LUT rebuilt\n", off_u32 & ~0xfffu);
}

/* ── erase-chip ────────────────────────────────────────────────────────────── */
static void cmd_erase_chip_L(void)
{
    printf("Erasing entire W77Q section 1 — this may take a while...\n");
    fflush(stdout);

    TEEC_Operation op = { 0 };
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE, TEEC_NONE, TEEC_NONE);

    uint32_t origin_u32;
    TEEC_Result res = TEEC_InvokeCommand(&g_sess_L, W77Q_DUMP_CMD_ERASE_CHIP,
                                         &op, &origin_u32);
    if (res != TEEC_SUCCESS)
        errx(1u, "ERASE_CHIP failed: 0x%x (origin_u32 0x%x)", res, origin_u32);

    printf("[OK] chip erased — all objects destroyed, LUT is empty\n");
}

/* ── read-file ─────────────────────────────────────────────────────────────── */
static void cmd_read_file_L(const char *ta_uuid_str, const char *obj_id_str)
{
    /* Parse ta_uuid hex string (36 chars with dashes or 32 chars plain) */
    uint8_t uuid_u8[16];
    const char *s = ta_uuid_str;
    uint32_t ui = 0;

    for (ui = 0; ui < 16u && *s != '\0'; ui++)
    {
        if (*s == '-') s++;
        char byte[3] = { s[0], s[1], 0 };
        uuid_u8[ui] = (uint8_t)strtoul(byte, NULL, 16u);
        s += 2u;
    }
    if (ui < 16u)
        errx(1u, "ta_uuid must be 32 hex chars (with or without dashes)");

    /* obj_id_str is a hex-encoded string of the raw object-ID bytes */
    size_t obj_id_hexlen = strlen(obj_id_str);
    if (obj_id_hexlen == 0 || (obj_id_hexlen % 2u) != 0 ||
        (obj_id_hexlen / 2u) > W77Q_DUMP_OBJ_ID_MAX)
        errx(1u, "obj_id must be 2..%u hex chars (even count)",
             W77Q_DUMP_OBJ_ID_MAX * 2u);

    uint32_t obj_id_len = (uint32_t)(obj_id_hexlen / 2u);
    uint8_t obj_id_bytes[W77Q_DUMP_OBJ_ID_MAX];
    for (uint32_t bi = 0; bi < obj_id_len; bi++)
    {
        char byte2[3] = { obj_id_str[bi * 2u], obj_id_str[bi * 2u + 1u], 0 };
        char *endp = NULL;
        obj_id_bytes[bi] = (uint8_t)strtoul(byte2, &endp, 16u);
        if (endp != byte2 + 2u)
            errx(1u, "obj_id contains non-hex characters");
    }

    /* Build input: uuid (16) || obj_id (variable) */
    uint32_t in_sz = 16u + obj_id_len;
    uint8_t *in_buf = malloc(in_sz);
    if (in_buf == NULL) err(1u, "malloc");
    memcpy(in_buf, uuid_u8, 16u);
    memcpy(in_buf + 16u, obj_id_bytes, obj_id_len);

    TEEC_SharedMemory shm_in = {
        .buffer = in_buf,
        .size   = in_sz,
        .flags  = TEEC_MEM_INPUT,
    };
    TEEC_Result res = TEEC_RegisterSharedMemory(&g_ctx_L, &shm_in);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_RegisterSharedMemory (in): 0x%x", res);

    /* Output buffer: up to 64 KB */
    uint32_t out_sz = MAX_TRANSFER_LEN;
    uint8_t *out_buf = malloc(out_sz);
    if (out_buf == NULL) err(1u, "malloc");

    TEEC_SharedMemory shm_out = {
        .buffer = out_buf,
        .size   = out_sz,
        .flags  = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT,
    };
    res = TEEC_RegisterSharedMemory(&g_ctx_L, &shm_out);
    if (res != TEEC_SUCCESS)
        errx(1u, "TEEC_RegisterSharedMemory (out): 0x%x", res);

    TEEC_Operation op = { 0 };
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE,
                                     TEEC_MEMREF_WHOLE,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].memref.parent = &shm_in;
    op.params[0].memref.offset = 0;
    op.params[0].memref.size   = in_sz;
    op.params[1].memref.parent = &shm_out;
    op.params[1].memref.offset = 0;
    op.params[1].memref.size   = out_sz;

    uint32_t origin_u32;
    res = TEEC_InvokeCommand(&g_sess_L, W77Q_DUMP_CMD_READ_FILE, &op, &origin_u32);
    if (res != TEEC_SUCCESS)
    {
        printf("[ERROR] read-file failed: 0x%x (origin 0x%x)\n", res, origin_u32);
        TEEC_ReleaseSharedMemory(&shm_in);
        TEEC_ReleaseSharedMemory(&shm_out);
        free(in_buf);
        free(out_buf);
        return;
    }

    uint32_t actual_sz = op.params[1].memref.size;
    printf("[OK] file content (%u bytes):\n", actual_sz);
    hex_dump_L(out_buf, actual_sz);

    TEEC_ReleaseSharedMemory(&shm_in);
    TEEC_ReleaseSharedMemory(&shm_out);
    free(in_buf);
    free(out_buf);
}

/* ── main ──────────────────────────────────────────────────────────────────── */
static void usage_L(void)
{
    fprintf(stderr,
        "W77Q Flash Dump Tool — direct w77q_fs LUT access via Pseudo-TA\n\n"
        "Usage: w77q-dump <command>\n\n"
        "Commands:\n"
        "  list-all                        List ALL flash objects (all TAs)\n"
        "  read-raw  <off_u32> <len_u32>           Hex-dump <len_u32> bytes at flash offset <off_u32>\n"
        "  read-plain <off_u32> <len_u32>          Attempt UNAUTHENTICATED read (demo: should fail)\n"
        "  read-file <ta_uuid> <obj_id>    Read file content through LittleFS by name\n"
        "  write-raw <off_u32> <hex|str> [s]   Write bytes at <off_u32>; prefix 0x for hex string,\n"
        "                                  append 's' flag for raw ASCII string\n"
        "  erase-sector <off_u32>              Erase the 4 KB sector containing <off_u32>\n"
        "  erase-chip                      Erase ALL of section 1 (destroys all objects!)\n"
        "\nExamples:\n"
        "  w77q-dump list-all\n"
        "  w77q-dump read-raw 0x1000 256\n"
        "  w77q-dump read-plain 0x1000 256    # should be DENIED by W77Q\n"
        "  w77q-dump read-file fd02c9da-306c-48c7-a49c-bbd827ae86ee token.db.0\n"
        "  w77q-dump write-raw 0x1000 0xDEADBEEF\n"
        "  w77q-dump write-raw 0x1000 'hello' s\n"
        "  w77q-dump erase-sector 0x1000\n"
        "  w77q-dump erase-chip\n");
    exit(1u);
}

/*-----------------------------------------------------------------------------------------------------------
                                             INTERFACE FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

int main(int argc, char *argv[])
{
    if (argc < MIN_ARGC_CMD) usage_L();

    open_session_L();

    const char *cmd = argv[1];
    if (strcmp(cmd, "list-all") == 0)
    {
        cmd_list_all_L();
    } else if (strcmp(cmd, "read-raw") == 0)
    {
        if (argc < MIN_ARGC_TWO_PARAMS) usage_L();
        uint32_t off_u32 = (uint32_t)strtoul(argv[2], NULL, 0u);
        uint32_t len_u32 = (uint32_t)strtoul(argv[3], NULL, 0u);
        if (len_u32 == 0 || len_u32 > MAX_TRANSFER_LEN)
            errx(1u, "len_u32 must be 1..65536");
        cmd_read_raw_L(off_u32, len_u32);
    } else if (strcmp(cmd, "read-plain") == 0)
    {
        if (argc < MIN_ARGC_TWO_PARAMS) usage_L();
        uint32_t off_u32 = (uint32_t)strtoul(argv[2], NULL, 0u);
        uint32_t len_u32 = (uint32_t)strtoul(argv[3], NULL, 0u);
        if (len_u32 == 0 || len_u32 > MAX_TRANSFER_LEN)
            errx(1u, "len_u32 must be 1..65536");
        cmd_read_plain_L(off_u32, len_u32);
    } else if (strcmp(cmd, "write-raw") == 0)
    {
        if (argc < MIN_ARGC_TWO_PARAMS) usage_L();
        uint32_t off_u32 = (uint32_t)strtoul(argv[2], NULL, 0u);
        const char *val = argv[3];
        int as_str = (argc >= MIN_ARGC_WRITE_FLAG && strcmp(argv[4], "s") == 0);
        uint8_t *data_pu8 = NULL;
        uint32_t len_u32  = 0;
        if (!as_str && strncmp(val, "0x", HEX_PREFIX_LEN) == 0)
        {
            /* hex string: 0xDEADBEEF... */
            val += HEX_PREFIX_LEN;
            size_t hlen = strlen(val);
            if (hlen == 0 || hlen % HEX_CHARS_PER_BYTE != 0)
                errx(1u, "hex string must have even length");
            len_u32  = (uint32_t)(hlen / 2u);
            data_pu8 = malloc(len_u32);
            if (data_pu8 == NULL) err(1u, "malloc");
            for (uint32_t i = 0; i < len_u32; i++)
            {
                char byte[3] = { val[i*2], val[i*2+1], 0 };
                data_pu8[i] = (uint8_t)strtoul(byte, NULL, 16u);
            }
        } else
        {
            /* treat as raw ASCII string */
            len_u32  = (uint32_t)strlen(val);
            data_pu8 = (uint8_t *)val;
        }
        if (len_u32 == 0 || len_u32 > MAX_TRANSFER_LEN)
            errx(1u, "write length must be 1..65536");
        cmd_write_raw_L(off_u32, data_pu8, len_u32);
        if (!as_str && strncmp(argv[3], "0x", HEX_PREFIX_LEN) == 0)
            free(data_pu8);
    } else if (strcmp(cmd, "erase-sector") == 0)
    {
        if (argc < MIN_ARGC_ONE_PARAM) usage_L();
        uint32_t off_u32 = (uint32_t)strtoul(argv[2], NULL, 0u);
        cmd_erase_sector_L(off_u32);
    } else if (strcmp(cmd, "erase-chip") == 0)
    {
        cmd_erase_chip_L();
    } else if (strcmp(cmd, "read-file") == 0)
    {
        if (argc < MIN_ARGC_TWO_PARAMS) usage_L();
        cmd_read_file_L(argv[2], argv[3]);
    } else
    {
        usage_L();
    }

    close_session_L();
    return 0u;
}
