/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef USER_TA_HEADER_DEFINES_H
#define USER_TA_HEADER_DEFINES_H

#include <w77q_fs_test_ta.h>

#define TA_UUID             W77Q_FS_TEST_TA_UUID
#define TA_FLAGS            (TA_FLAG_EXEC_DDR | \
			     TA_FLAG_SINGLE_INSTANCE | \
			     TA_FLAG_MULTI_SESSION)
#define TA_STACK_SIZE       (32 * 1024)
#define TA_DATA_SIZE        (128 * 1024)
#define TA_VERSION          "1.0"
#define TA_DESCRIPTION      "w77q_fs filesystem test TA"

#endif /* USER_TA_HEADER_DEFINES_H */
