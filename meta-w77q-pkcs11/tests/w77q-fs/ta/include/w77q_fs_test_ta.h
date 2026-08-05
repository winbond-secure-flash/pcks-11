/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * w77q_fs_test_ta.h — shared header for w77q-fs-test TA and host app.
 *
 * TA UUID: f0e1d2c3-b4a5-9687-8796-a5b4c3d2e1f0
 *
 * Each command runs one self-contained test case inside the TA.  The TA
 * creates any objects it needs, exercises the requested w77q_fs code path,
 * cleans up, and returns TEE_SUCCESS on pass or a TEE error on failure.
 *
 * TC_ALL runs every individual test in sequence and returns a 32-bit
 * bitmask of failures in params[0].value.a (bit N set = TC_N failed).
 */
#ifndef W77Q_FS_TEST_TA_H
#define W77Q_FS_TEST_TA_H

/* ---- UUID ---------------------------------------------------------------- */
#define W77Q_FS_TEST_TA_UUID \
	{ 0xf0e1d2c3, 0xb4a5, 0x9687, \
	  { 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0 } }

/* ---- Commands (params[0].value.a carries extra info on TC_ALL) ----------- */
#define W77Q_FS_TC_BASIC	0U  /* create / read-back / delete          */
#define W77Q_FS_TC_UPDATE	1U  /* write, close, reopen, overwrite-write */
#define W77Q_FS_TC_RENAME	2U  /* rename; old gone, new readable        */
#define W77Q_FS_TC_ENUM		3U  /* create N, enumerate, verify count     */
#define W77Q_FS_TC_LARGE	4U  /* 8 KB payload, full read-back verify   */
#define W77Q_FS_TC_OVERWRITE	5U  /* create-with-overwrite replaces data   */
#define W77Q_FS_TC_TRUNCATE	6U  /* truncate to half; verify length       */
#define W77Q_FS_TC_ALL		7U  /* run TC 0-6; failure mask in out[0].a  */
#define W77Q_FS_TC_DIAG_CREATE	  8U  /* create wft.diag — leave for inspection */
#define W77Q_FS_TC_DIAG_DELETE	  9U  /* delete wft.diag                        */
#define W77Q_FS_TC_DIAG_SETUP	 10U  /* pre-create TC[n]'s objects; params[0].value.a = tc_idx */
#define W77Q_FS_TC_DIAG_TEARDOWN 11U  /* delete any objects owned by TC[n]      */

#define W77Q_FS_TC_COUNT	7U  /* number of individual test cases       */

/* ---- Object ID prefix used by every test case --------------------------- */
#define W77Q_FS_TEST_PREFIX     "wft."

/* ---- Payload sizes ------------------------------------------------------- */
#define W77Q_FS_TEST_BASIC_SZ	32U
#define W77Q_FS_TEST_LARGE_SZ	8192U
#define W77Q_FS_TEST_TRUNC_SZ	256U
#define W77Q_FS_TEST_ENUM_CNT	10U

#endif /* W77Q_FS_TEST_TA_H */
