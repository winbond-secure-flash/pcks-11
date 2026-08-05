/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       main.c
* @brief      TEE Storage Client Application (Normal World) — CLI for W77Q secure storage
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
#include <unistd.h>

#include <tee_client_api.h>
#include "tee_storage_ta.h"

/*-----------------------------------------------------------------------------------------------------------
                                                 DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

#define HEX_DUMP_BYTES_PER_ROW   16u
#define HEX_DUMP_LAST_COL        15u
#define SECTOR_ID_LEN            9u
#define MIN_ARGC_CMD             2u
#define MIN_ARGC_ID_CMD          3u
#define MIN_ARGC_DATA_CMD        4u
#define MIN_ARGC_FILE_CMD        5u

/*-----------------------------------------------------------------------------------------------------------
                                               LOCAL VARIABLES
-----------------------------------------------------------------------------------------------------------*/

static TEEC_Context g_ctx_L;
static TEEC_Session g_sess_L;

/*-----------------------------------------------------------------------------------------------------------
                                          LOCAL FUNCTION PROTOTYPES
-----------------------------------------------------------------------------------------------------------*/

static void open_session_L(void);
static void close_session_L(void);
static uint8_t *read_file_L(const char *path, size_t *out_len);
static void hex_dump_L(const uint8_t *buf_pu8, size_t len);
static void print_data_L(const uint8_t *buf_pu8, size_t len);
static const char *tee_strerror_L(uint32_t res_u32);
static int32_t do_write_L(const char *id, const uint8_t *data_pu8, size_t data_len);
static int32_t do_read_L(const char *id);
static int32_t do_erase_L(const char *id);
static int32_t do_list_L(void);
static int32_t do_write_sector_L(uint32_t addr_u32, const uint8_t *data_pu8, size_t data_len);
static int32_t do_read_sector_L(uint32_t addr_u32);
static int32_t do_erase_sector_L(uint32_t addr_u32);
static int32_t do_format_L(void);
static int32_t do_info_L(void);
static void usage_L(const char *prog);

/*-----------------------------------------------------------------------------------------------------------
                                               LOCAL FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

/* ── Session management ──────────────────────────────────────────────────── */

static void open_session_L(void)
{
    TEEC_UUID uuid = TA_TEE_STORAGE_UUID;
    uint32_t  err_origin_u32;

    TEEC_Result res_u32 = TEEC_InitializeContext(NULL, &g_ctx_L);
    if (res_u32 != TEEC_SUCCESS)
        errx(1u, "TEEC_InitializeContext failed: 0x%x", res_u32);

    res_u32 = TEEC_OpenSession(&g_ctx_L, &g_sess_L, &uuid,
                           TEEC_LOGIN_PUBLIC, NULL, NULL, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        TEEC_FinalizeContext(&g_ctx_L);
        errx(1u, "TEEC_OpenSession failed: 0x%x (origin 0x%x)",
             res_u32, err_origin_u32);
    }
}

static void close_session_L(void)
{
    TEEC_CloseSession(&g_sess_L);
    TEEC_FinalizeContext(&g_ctx_L);
}

/* ── Utilities ───────────────────────────────────────────────────────────── */

/* Read an entire file into a malloc'd buffer. Caller must free(). */
static uint8_t *read_file_L(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        err(1u, "fopen: %s", path);

    if (fseek(f, 0u, SEEK_END) < 0)
        err(1u, "fseek: %s", path);

    int32_t sz_i32 = (int32_t)ftell(f);
    if (sz_i32 < 0)
        err(1u, "ftell: %s", path);
    if ((size_t)sz_i32 > TS_MAX_DATA_LEN)
        errx(1u, "%s: file too large (%d bytes, max %d)", path, sz_i32, TS_MAX_DATA_LEN);

    rewind(f);

    uint8_t *buf_pu8 = malloc((size_t)sz_i32 + 1u);
    if (buf_pu8 == NULL)
        err(1u, "malloc");

    if (fread(buf_pu8, 1u, (size_t)sz_i32, f) != (size_t)sz_i32)
        err(1u, "fread: %s", path);

    fclose(f);
    *out_len = (size_t)sz_i32;
    return buf_pu8;
}

/* Hex dump — 16 bytes per row with offset prefix. */
static void hex_dump_L(const uint8_t *buf_pu8, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (i % HEX_DUMP_BYTES_PER_ROW == 0)
            printf("%04zx  ", i);
        printf("%02x ", buf_pu8[i]);
        if (i % HEX_DUMP_BYTES_PER_ROW == HEX_DUMP_LAST_COL || i == len - 1u)
        {
            /* pad incomplete final row */
            if (i % HEX_DUMP_BYTES_PER_ROW != HEX_DUMP_LAST_COL)
                for (size_t p = (i % HEX_DUMP_BYTES_PER_ROW) + 1u; p < HEX_DUMP_BYTES_PER_ROW; p++)
                    printf("   ");
            printf(" |");
            for (size_t j = i - (i % HEX_DUMP_BYTES_PER_ROW); j <= i; j++)
                printf("%c", (buf_pu8[j] >= 0x20 && buf_pu8[j] <= 0x7e) ? buf_pu8[j] : '.');
            printf("|\n");
        }
    }
}

/* Print data as text if fully printable ASCII, otherwise hex dump. */
static void print_data_L(const uint8_t *buf_pu8, size_t len)
{
    int32_t printable_i32 = 1;
    for (size_t i = 0; i < len; i++)
    {
        if (buf_pu8[i] < 0x20 || buf_pu8[i] > 0x7e)
        {
            printable_i32 = 0;
            break;
        }
    }
    if (printable_i32)
        printf("%.*s\n", (int32_t)len, buf_pu8);
    else
        hex_dump_L(buf_pu8, len);
}

/* TEE error -> human-readable string for common codes. */
static const char *tee_strerror_L(uint32_t res_u32)
{
    switch (res_u32)
    {
    case 0xFFFF0000: return "TEE_ERROR_GENERIC";
    case 0xFFFF0001: return "TEE_ERROR_ACCESS_DENIED";
    case 0xFFFF0002: return "TEE_ERROR_CANCEL";
    case 0xFFFF0003: return "TEE_ERROR_ACCESS_CONFLICT";
    case 0xFFFF0004: return "TEE_ERROR_EXCESS_DATA";
    case 0xFFFF0005: return "TEE_ERROR_BAD_FORMAT";
    case 0xFFFF0006: return "TEE_ERROR_BAD_PARAMETERS";
    case 0xFFFF0007: return "TEE_ERROR_BAD_STATE";
    case 0xFFFF0008: return "TEE_ERROR_ITEM_NOT_FOUND";
    case 0xFFFF000A: return "TEE_ERROR_NOT_SUPPORTED";
    case 0xFFFF000B: return "TEE_ERROR_NO_DATA";
    case 0xFFFF000C: return "TEE_ERROR_OUT_OF_MEMORY";
    case 0xFFFF000D: return "TEE_ERROR_BUSY";
    case 0xFFFF000E: return "TEE_ERROR_COMMUNICATION";
    case 0xFFFF000F: return "TEE_ERROR_SECURITY";
    case 0xFFFF0010: return "TEE_ERROR_SHORT_BUFFER";
    case 0xFFFF3024: return "TEE_ERROR_TARGET_DEAD";
    case 0xFFFF3041: return "TEE_ERROR_STORAGE_NO_SPACE";
    case 0xF0100001: return "TEE_ERROR_CORRUPT_OBJECT";
    case 0xF0100003: return "TEE_ERROR_STORAGE_NOT_AVAILABLE";
    default:         return "unknown error";
    }
}

/* ── Named-object commands ───────────────────────────────────────────────── */

static int32_t do_write_L(const char *id, const uint8_t *data_pu8, size_t data_len)
{
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
                                     TEEC_MEMREF_TEMP_INPUT,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].tmpref.buffer = (void *)id;
    op.params[0].tmpref.size   = strlen(id);
    op.params[1].tmpref.buffer = (void *)data_pu8;
    op.params[1].tmpref.size   = data_len;

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_WRITE, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "write failed: 0x%x (%s), origin 0x%x\n",
                res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }
    printf("[OK] wrote %zu bytes to object '%s'\n", data_len, id);
    return 0u;
}

static int32_t do_read_L(const char *id)
{
    static uint8_t buf_u8[TS_MAX_DATA_LEN];
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
                                     TEEC_MEMREF_TEMP_INOUT,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].tmpref.buffer = (void *)id;
    op.params[0].tmpref.size   = strlen(id);
    op.params[1].tmpref.buffer = buf_u8;
    op.params[1].tmpref.size   = sizeof(buf_u8);

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_READ, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "read '%s' failed: 0x%x (%s), origin 0x%x\n",
                id, res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }

    size_t got = op.params[1].tmpref.size;
    printf("[OK] object '%s': %zu bytes\n", id, got);
    print_data_L(buf_u8, got);
    return 0u;
}

static int32_t do_erase_L(const char *id)
{
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
                                     TEEC_NONE, TEEC_NONE, TEEC_NONE);
    op.params[0].tmpref.buffer = (void *)id;
    op.params[0].tmpref.size   = strlen(id);

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_ERASE, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "erase '%s' failed: 0x%x (%s), origin 0x%x\n",
                id, res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }
    printf("[OK] erased object '%s'\n", id);
    return 0u;
}

static int32_t do_list_L(void)
{
    static char buf_u8[TS_MAX_LIST_BUF];
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_OUTPUT,
                                     TEEC_VALUE_OUTPUT,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].tmpref.buffer = buf_u8;
    op.params[0].tmpref.size   = sizeof(buf_u8);

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_LIST, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "list failed: 0x%x (%s), origin 0x%x\n",
                res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }

    uint32_t count_u32 = op.params[1].value.a;
    size_t   used  = op.params[0].tmpref.size;

    printf("[OK] %u object(s) in TEE secure storage:\n", count_u32);

    const char *p   = buf_u8;
    const char *end = buf_u8 + used;
    while (p < end)
    {
        size_t len = strnlen(p, (size_t)(end - p));
        /* Distinguish sector objects from named objects */
        if (len == SECTOR_ID_LEN && p[0] == 'S')
        {
            /* check all 8 suffix chars are hex */
            int32_t is_sector_i32 = 1;
            for (uint32_t i_u32 = 1u; i_u32 < SECTOR_ID_LEN; i_u32++)
            {
                char c = p[i_u32];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    is_sector_i32 = 0;
            }
            if (is_sector_i32)
            {
                uint32_t addr_u32 = (uint32_t)strtoul(p + 1u, NULL, 16u);
                printf("  [sector] 0x%08x\n", addr_u32);
                p += len + 1u;
                continue;
            }
        }
        printf("  [object] %.*s\n", (int32_t)len, p);
        p += len + 1u;
    }
    return 0u;
}

/* ── Sector commands ─────────────────────────────────────────────────────── */

static int32_t do_write_sector_L(uint32_t addr_u32, const uint8_t *data_pu8, size_t data_len)
{
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
                                     TEEC_MEMREF_TEMP_INPUT,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].value.a       = addr_u32;
    op.params[1].tmpref.buffer = (void *)data_pu8;
    op.params[1].tmpref.size   = data_len;

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_WRITE_SECTOR, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "write-sector 0x%08x failed: 0x%x (%s), origin 0x%x\n",
                addr_u32, res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }
    printf("[OK] wrote %zu bytes to sector 0x%08x\n", data_len, addr_u32);
    return 0u;
}

static int32_t do_read_sector_L(uint32_t addr_u32)
{
    /* Full sector buffer — zero-padded so the dump always shows TS_SECTOR_SIZE bytes. */
    static uint8_t buf_u8[TS_SECTOR_SIZE];
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    memset(buf_u8, 0, sizeof(buf_u8));

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
                                     TEEC_MEMREF_TEMP_INOUT,
                                     TEEC_NONE, TEEC_NONE);
    op.params[0].value.a       = addr_u32;
    op.params[1].tmpref.buffer = buf_u8;
    op.params[1].tmpref.size   = sizeof(buf_u8);

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_READ_SECTOR, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "read-sector 0x%08x failed: 0x%x (%s), origin 0x%x\n",
                addr_u32, res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }

    size_t got = op.params[1].tmpref.size;
    printf("[OK] sector 0x%08x: %zu bytes stored, %zu bytes dumped\n",
           addr_u32, got, (size_t)TS_SECTOR_SIZE);
    hex_dump_L(buf_u8, TS_SECTOR_SIZE);
    return 0u;
}

static int32_t do_erase_sector_L(uint32_t addr_u32)
{
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
                                     TEEC_NONE, TEEC_NONE, TEEC_NONE);
    op.params[0].value.a = addr_u32;

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_ERASE_SECTOR, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "erase-sector 0x%08x failed: 0x%x (%s), origin 0x%x\n",
                addr_u32, res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }
    printf("[OK] erased sector 0x%08x\n", addr_u32);
    return 0u;
}

/* ── Storage-level commands ──────────────────────────────────────────────── */

static int32_t do_format_L(void)
{
    printf("Formatting TEE secure storage (erasing ALL objects)...\n");
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE, TEEC_NONE, TEEC_NONE);

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_FORMAT, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "format failed: 0x%x (%s), origin 0x%x\n",
                res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }
    printf("[OK] TEE secure storage formatted\n");
    return 0u;
}

static int32_t do_info_L(void)
{
    TEEC_Operation op = { 0 };
    uint32_t err_origin_u32;

    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT,
                                     TEEC_VALUE_OUTPUT,
                                     TEEC_NONE, TEEC_NONE);

    TEEC_Result res_u32 = TEEC_InvokeCommand(&g_sess_L, CMD_INFO, &op, &err_origin_u32);
    if (res_u32 != TEEC_SUCCESS)
    {
        fprintf(stderr, "info failed: 0x%x (%s), origin 0x%x\n",
                res_u32, tee_strerror_L(res_u32), err_origin_u32);
        return 1u;
    }

    printf("[OK] TEE Secure Storage Info (W77Q backend)\n");
    printf("     Objects : %u\n",    op.params[0].value.a);
    printf("     Data    : %u bytes\n", op.params[1].value.a);
    return 0u;
}

/* ── Usage ───────────────────────────────────────────────────────────────── */

static void usage_L(const char *prog)
{
    fprintf(stderr,
        "TEE Storage Utility — userspace read/write/erase/format\n"
        "Backend: OP-TEE TEE_STORAGE_PRIVATE (W77Q SPI flash on Sparrow Hawk)\n"
        "\n"
        "Usage: %s <command> [args]\n"
        "\n"
        "Named-object commands:\n"
        "  write  <id> <data>         Write string to named TEE object\n"
        "  write  <id> -f <file>      Write file contents to named TEE object\n"
        "  read   <id>                Read and display named TEE object\n"
        "  erase  <id>                Delete named TEE object\n"
        "  list                       List all objects in TEE storage\n"
        "\n"
        "Sector commands (addr = decimal or 0x-prefixed hex):\n"
        "  write-sector <addr> <data>       Write string to sector address\n"
        "  write-sector <addr> -f <file>    Write file to sector address\n"
        "  read-sector  <addr>              Read and hex-dump sector\n"
        "  erase-sector <addr>              Erase sector\n"
        "\n"
        "Storage commands:\n"
        "  format                     Erase ALL objects — destructive!\n"
        "  info                       Show object count and total bytes\n"
        "\n"
        "Examples:\n"
        "  %s write config 'device-id=SH001'\n"
        "  %s read  config\n"
        "  %s write-sector 0x1000 'sector data'\n"
        "  %s write-sector 0x2000 -f /etc/hostname\n"
        "  %s read-sector  0x1000\n"
        "  %s erase-sector 0x1000\n"
        "  %s list\n"
        "  %s info\n"
        "  %s format\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
    exit(1u);
}

/*-----------------------------------------------------------------------------------------------------------
                                             INTERFACE FUNCTIONS
-----------------------------------------------------------------------------------------------------------*/

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < (int32_t)MIN_ARGC_CMD)
        usage_L(argv[0]);

    const char *cmd = argv[1];
    int32_t rc_i32 = 0;

    open_session_L();

    if (strcmp(cmd, "write") == 0)
    {
        if (argc < (int32_t)MIN_ARGC_DATA_CMD) usage_L(argv[0]);
        const char *id = argv[2];
        uint8_t *data_pu8;
        size_t   data_len;
        int32_t  need_free_i32 = 0;

        if (strcmp(argv[3], "-f") == 0)
        {
            if (argc < (int32_t)MIN_ARGC_FILE_CMD) usage_L(argv[0]);
            data_pu8 = read_file_L(argv[4], &data_len);
            need_free_i32 = 1;
        } else
        {
            data_pu8     = (uint8_t *)argv[3];
            data_len = strlen(argv[3]);
        }
        rc_i32 = do_write_L(id, data_pu8, data_len);
        if (need_free_i32) free(data_pu8);

    } else if (strcmp(cmd, "read") == 0)
    {
        if (argc < (int32_t)MIN_ARGC_ID_CMD) usage_L(argv[0]);
        rc_i32 = do_read_L(argv[2]);

    } else if (strcmp(cmd, "erase") == 0)
    {
        if (argc < (int32_t)MIN_ARGC_ID_CMD) usage_L(argv[0]);
        rc_i32 = do_erase_L(argv[2]);

    } else if (strcmp(cmd, "list") == 0)
    {
        rc_i32 = do_list_L();

    } else if (strcmp(cmd, "write-sector") == 0)
    {
        if (argc < (int32_t)MIN_ARGC_DATA_CMD) usage_L(argv[0]);
        char *endp;
        uint64_t addr_u64 = (uint64_t)strtoul(argv[2], &endp, 0u);
        if (*endp)
            errx(1u, "invalid sector address: %s", argv[2]);
        if (addr_u64 > UINT32_MAX)
            errx(1u, "sector address out of range: %s", argv[2]);
        uint32_t addr_u32 = (uint32_t)addr_u64;

        uint8_t *data_pu8;
        size_t   data_len;
        int32_t  need_free_i32 = 0;

        if (strcmp(argv[3], "-f") == 0)
        {
            if (argc < (int32_t)MIN_ARGC_FILE_CMD) usage_L(argv[0]);
            data_pu8 = read_file_L(argv[4], &data_len);
            need_free_i32 = 1;
        } else
        {
            data_pu8     = (uint8_t *)argv[3];
            data_len = strlen(argv[3]);
        }
        rc_i32 = do_write_sector_L(addr_u32, data_pu8, data_len);
        if (need_free_i32) free(data_pu8);

    } else if (strcmp(cmd, "read-sector") == 0)
    {
        if (argc < (int32_t)MIN_ARGC_ID_CMD) usage_L(argv[0]);
        char *endp;
        uint64_t addr_u64 = (uint64_t)strtoul(argv[2], &endp, 0u);
        if (*endp)
            errx(1u, "invalid sector address: %s", argv[2]);
        if (addr_u64 > UINT32_MAX)
            errx(1u, "sector address out of range: %s", argv[2]);
        rc_i32 = do_read_sector_L((uint32_t)addr_u64);

    } else if (strcmp(cmd, "erase-sector") == 0)
    {
        if (argc < (int32_t)MIN_ARGC_ID_CMD) usage_L(argv[0]);
        char *endp;
        uint64_t addr_u64 = (uint64_t)strtoul(argv[2], &endp, 0u);
        if (*endp)
            errx(1u, "invalid sector address: %s", argv[2]);
        if (addr_u64 > UINT32_MAX)
            errx(1u, "sector address out of range: %s", argv[2]);
        rc_i32 = do_erase_sector_L((uint32_t)addr_u64);

    } else if (strcmp(cmd, "format") == 0)
    {
        rc_i32 = do_format_L();

    } else if (strcmp(cmd, "info") == 0)
    {
        rc_i32 = do_info_L();

    } else
    {
        close_session_L();
        usage_L(argv[0]);
    }

    close_session_L();
    return rc_i32;
}
