/************************************************************************************************************
* @internal
* @remark     Winbond - Confidential
* @copyright  Copyright (c) 2026 by Winbond. All rights reserved
* @endinternal
*
* @file       lfs_w77q_cfg.h
* @brief      LittleFS compile-time configuration overrides for OP-TEE / W77Q
*
* ### project meta-w77q-pkcs11
*
* Included via -DLFS_DEFINES=lfs_w77q_cfg.h.  This header is parsed inside
* lfs_util.h BEFORE the default macros, so any #define here takes priority.
*
************************************************************************************************************/

#ifndef LFS_W77Q_CFG_H__
#define LFS_W77Q_CFG_H__

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------------------------------------------
                                                 DEFINITIONS
-----------------------------------------------------------------------------------------------------------*/

/* ---- Static allocation -------------------------------------------------- */
/* Disable LittleFS internal malloc/free — all buffers are statically          */
/* allocated and passed via lfs_config / lfs_file_config.                      */
#define LFS_NO_MALLOC

/* ---- Assertions --------------------------------------------------------- */
/* OP-TEE provides assert() via <assert.h>, but in kernel context calling      */
/* abort() is not safe.  Use OP-TEE's TEE_ASSERT / panic() instead.            */
#include <assert.h>
#define LFS_ASSERT(test)  assert(test)

/* ---- Disable stdio ------------------------------------------------------ */
/* Define LFS_NO_* BEFORE lfs_util.h checks them, to prevent <stdio.h>.       */
#define LFS_NO_DEBUG
#define LFS_NO_WARN
#define LFS_NO_ERROR

/* ---- Logging ------------------------------------------------------------ */
/* Map LittleFS log macros to OP-TEE trace macros.  These override the         */
/* defaults in lfs_util.h via the #ifndef guards.                              */
#include <trace.h>

#define LFS_TRACE(...)
#define LFS_DEBUG(fmt, ...)  DMSG("lfs: " fmt, ##__VA_ARGS__)
#define LFS_WARN(fmt, ...)   IMSG("lfs: " fmt, ##__VA_ARGS__)
#define LFS_ERROR(fmt, ...)  EMSG("lfs: " fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LFS_W77Q_CFG_H__ */
