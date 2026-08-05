/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * user_ta_header_defines.h — OP-TEE TA metadata for the W77Q demo TA.
 * Processed by the OP-TEE devkit to generate the signed TA binary header.
 */
#ifndef USER_TA_HEADER_DEFINES_H
#define USER_TA_HEADER_DEFINES_H

#include <tee_demo_ta.h>

#define TA_UUID             DEMO_TA_UUID

/*
 * TA_FLAG_EXEC_DDR        — load TA into DDR (not SRAM)
 * TA_FLAG_SINGLE_INSTANCE — one instance shared across all sessions
 * TA_FLAG_MULTI_SESSION   — allow multiple simultaneous CA sessions
 */
#define TA_FLAGS            (TA_FLAG_EXEC_DDR | \
			     TA_FLAG_SINGLE_INSTANCE | \
			     TA_FLAG_MULTI_SESSION)

#define TA_STACK_SIZE       (8 * 1024)   /* 8 KB — power-safe R/W + crypto ops */
#define TA_DATA_SIZE        (64 * 1024)  /* 64 KB heap                       */

#define TA_VERSION          "1.0"
#define TA_DESCRIPTION      "W77Q QLIB secure storage demo TA"

#endif /* USER_TA_HEADER_DEFINES_H */
