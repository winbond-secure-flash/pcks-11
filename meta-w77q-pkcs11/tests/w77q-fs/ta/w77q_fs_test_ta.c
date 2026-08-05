// SPDX-License-Identifier: BSD-2-Clause
/*
 * w77q_fs_test_ta.c — w77q_fs filesystem test Trusted Application.
 *
 * Runs self-contained test cases that exercise every w77q_fs code path via
 * the GP TEE Internal API (TEE_STORAGE_PRIVATE).  Each TC_* command:
 *   - creates all objects it needs from scratch,
 *   - exercises the target operation,
 *   - deletes everything it created,
 *   - returns TEE_SUCCESS on pass or the failing TEE error code.
 *
 * TC_ALL (cmd 7) runs TC 0-6 in sequence and returns a 32-bit failure
 * bitmask in params[0].value.a (bit N set means TC N failed).
 */

#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <string.h>

#include "w77q_fs_test_ta.h"

/* ---- Object ID helpers -------------------------------------------------- */

/* All test objects use IDs of the form "wft.<tag>[.NN]" to avoid colliding
 * with objects created by other TAs. */

static const uint32_t STORAGE = TEE_STORAGE_PRIVATE;

/* Open flags used frequently */
#define FL_RW  (TEE_DATA_FLAG_ACCESS_READ  | TEE_DATA_FLAG_ACCESS_WRITE)
#define FL_RD   TEE_DATA_FLAG_ACCESS_READ
#define FL_WR   TEE_DATA_FLAG_ACCESS_WRITE

/* ---- Logging helpers ----------------------------------------------------- */

#define PASS_IF(cond, label) \
	do { if (!(cond)) { EMSG("FAIL %s:%d", __func__, __LINE__); goto label; } } while (0)

#define CHECK(res, label) \
	do { if ((res) != TEE_SUCCESS) { \
		EMSG("FAIL %s:%d res=0x%08x", __func__, __LINE__, (res)); \
		goto label; } } while (0)

/* ---- TC 0: BASIC (create / read-back / delete) -------------------------- */
/*
 * Creates one 32-byte object, reads it back byte-for-byte, then deletes it.
 * Exercises: TEE_CreatePersistentObject, TEE_ReadObjectData, TEE_CloseObject,
 *            TEE_OpenPersistentObject, TEE_DeletePersistentObject.
 */
static TEE_Result tc_basic(void)
{
	static const char  id[]  = "wft.basic";
	static const char  data[] = "w77q_fs_test_basic_payload_32B!";
	TEE_ObjectHandle   obj   = TEE_HANDLE_NULL;
	TEE_Result         res   = TEE_SUCCESS;
	char               buf[W77Q_FS_TEST_BASIC_SZ + 1];
	size_t             rd    = 0;

	/* --- create -------------------------------------------------------- */
	res = TEE_CreatePersistentObject(STORAGE,
					 id, sizeof(id) - 1,
					 FL_RW | TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 data, sizeof(data) - 1,
					 &obj);
	CHECK(res, out);
	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* --- open + read --------------------------------------------------- */
	res = TEE_OpenPersistentObject(STORAGE,
				       id, sizeof(id) - 1,
				       FL_RD, &obj);
	CHECK(res, out);

	TEE_MemFill(buf, 0, sizeof(buf));
	res = TEE_ReadObjectData(obj, buf, sizeof(buf) - 1, &rd);
	CHECK(res, cleanup);

	PASS_IF(rd == sizeof(data) - 1, cleanup_fail);
	PASS_IF(TEE_MemCompare(buf, data, rd) == 0, cleanup_fail);

	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* --- delete -------------------------------------------------------- */
	res = TEE_OpenPersistentObject(STORAGE,
				       id, sizeof(id) - 1,
				       FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				       &obj);
	CHECK(res, out);
	res = TEE_CloseAndDeletePersistentObject1(obj);  /* closes handle */
	obj = TEE_HANDLE_NULL;
	CHECK(res, out);

	/* --- verify gone --------------------------------------------------- */
	res = TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1, FL_RD, &obj);
	PASS_IF(res == TEE_ERROR_ITEM_NOT_FOUND, out_bad);
	res = TEE_SUCCESS;
	goto out;

cleanup_fail:
	res = TEE_ERROR_GENERIC;
cleanup:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	/* best-effort cleanup */
	if (TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				     FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				     &obj) == TEE_SUCCESS)
		TEE_CloseAndDeletePersistentObject1(obj);
	return res;

out_bad:
	res = TEE_ERROR_GENERIC;
out:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	return res;
}

/* ---- TC 1: UPDATE (close / reopen / write new data) --------------------- */
/*
 * Creates object with payload A, closes it, reopens write-only, writes
 * payload B, closes, reopens read-only, verifies content is B.
 * Exercises: TEE_WriteObjectData on an existing open object.
 */
static TEE_Result tc_update(void)
{
	static const char  id[]  = "wft.update";
	static const char  dataA[] = "payload-AAAA-initial-32bytes!!!";
	static const char  dataB[] = "payload-BBBB-updated-32bytes!!!";
	TEE_ObjectHandle   obj   = TEE_HANDLE_NULL;
	TEE_Result         res   = TEE_SUCCESS;
	char               buf[W77Q_FS_TEST_BASIC_SZ + 1];
	size_t             rd    = 0;

	/* create with dataA */
	res = TEE_CreatePersistentObject(STORAGE,
					 id, sizeof(id) - 1,
					 FL_RW | TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 dataA, sizeof(dataA) - 1,
					 &obj);
	CHECK(res, out);
	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* reopen write, overwrite with dataB */
	res = TEE_OpenPersistentObject(STORAGE,
				       id, sizeof(id) - 1,
				       FL_WR, &obj);
	CHECK(res, cleanup);

	res = TEE_WriteObjectData(obj, dataB, sizeof(dataB) - 1);
	CHECK(res, cleanup);
	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* reopen read, verify dataB */
	res = TEE_OpenPersistentObject(STORAGE,
				       id, sizeof(id) - 1,
				       FL_RD, &obj);
	CHECK(res, cleanup);

	TEE_MemFill(buf, 0, sizeof(buf));
	res = TEE_ReadObjectData(obj, buf, sizeof(buf) - 1, &rd);
	CHECK(res, cleanup);

	PASS_IF(rd == sizeof(dataB) - 1, cleanup_fail);
	PASS_IF(TEE_MemCompare(buf, dataB, rd) == 0, cleanup_fail);

	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* delete */
	res = TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				       FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				       &obj);
	CHECK(res, out);
	res = TEE_CloseAndDeletePersistentObject1(obj);
	obj = TEE_HANDLE_NULL;
	goto out;

cleanup_fail:
	res = TEE_ERROR_GENERIC;
cleanup:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	if (TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				     FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				     &obj) == TEE_SUCCESS)
		TEE_CloseAndDeletePersistentObject1(obj);
	return res;
out:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	return res;
}

/* ---- TC 2: RENAME -------------------------------------------------------- */
/*
 * Creates "wft.ren.src", renames to "wft.ren.dst", verifies old ID is gone
 * (NOT_FOUND) and new ID returns correct data.
 * Exercises: TEE_RenamePersistentObject.
 */
static TEE_Result tc_rename(void)
{
	static const char  src_id[]  = "wft.ren.src";
	static const char  dst_id[]  = "wft.ren.dst";
	static const char  data[]    = "rename-test-payload";
	TEE_ObjectHandle   obj       = TEE_HANDLE_NULL;
	TEE_Result         res       = TEE_SUCCESS;
	char               buf[32];
	size_t             rd        = 0;

	/* create src */
	res = TEE_CreatePersistentObject(STORAGE,
					 src_id, sizeof(src_id) - 1,
					 FL_RW | TEE_DATA_FLAG_OVERWRITE |
					 TEE_DATA_FLAG_ACCESS_WRITE_META,
					 TEE_HANDLE_NULL,
					 data, sizeof(data) - 1,
					 &obj);
	CHECK(res, out);

	/* rename src → dst (handle stays open) */
	res = TEE_RenamePersistentObject(obj, dst_id, sizeof(dst_id) - 1);
	CHECK(res, cleanup);
	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* verify src is gone */
	res = TEE_OpenPersistentObject(STORAGE,
				       src_id, sizeof(src_id) - 1,
				       FL_RD, &obj);
	PASS_IF(res == TEE_ERROR_ITEM_NOT_FOUND, out_bad);

	/* open dst, verify data */
	res = TEE_OpenPersistentObject(STORAGE,
				       dst_id, sizeof(dst_id) - 1,
				       FL_RD, &obj);
	CHECK(res, out);

	TEE_MemFill(buf, 0, sizeof(buf));
	res = TEE_ReadObjectData(obj, buf, sizeof(buf), &rd);
	CHECK(res, cleanup);

	PASS_IF(rd == sizeof(data) - 1, cleanup_fail);
	PASS_IF(TEE_MemCompare(buf, data, rd) == 0, cleanup_fail);

	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* delete dst */
	res = TEE_OpenPersistentObject(STORAGE, dst_id, sizeof(dst_id) - 1,
				       FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				       &obj);
	CHECK(res, out);
	res = TEE_CloseAndDeletePersistentObject1(obj);
	obj = TEE_HANDLE_NULL;
	goto out;

cleanup_fail:
	res = TEE_ERROR_GENERIC;
cleanup:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	if (TEE_OpenPersistentObject(STORAGE, src_id, sizeof(src_id) - 1,
				     FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				     &obj) == TEE_SUCCESS)
		TEE_CloseAndDeletePersistentObject1(obj);
	if (TEE_OpenPersistentObject(STORAGE, dst_id, sizeof(dst_id) - 1,
				     FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				     &obj) == TEE_SUCCESS)
		TEE_CloseAndDeletePersistentObject1(obj);
	return res;

out_bad:
	res = TEE_ERROR_GENERIC;
out:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	return res;
}

/* ---- TC 3: ENUM (opendir / readdir) ------------------------------------- */
/*
 * Creates W77Q_FS_TEST_ENUM_CNT (10) objects "wft.enum.00".."wft.enum.09",
 * runs the object enumerator and counts how many "wft.enum." objects are
 * found (must equal W77Q_FS_TEST_ENUM_CNT), then deletes all of them.
 * Exercises: TEE_AllocatePersistentObjectEnumerator,
 *            TEE_StartPersistentObjectEnumerator,
 *            TEE_GetNextPersistentObject.
 */
static TEE_Result tc_enum(void)
{
	static const char  prefix[] = "wft.enum.";
	const size_t       pfx_len  = sizeof(prefix) - 1; /* 9 */
	char               id[16];
	TEE_ObjectHandle   obj      = TEE_HANDLE_NULL;
	TEE_ObjectEnumHandle en     = TEE_HANDLE_NULL;
	TEE_Result         res      = TEE_SUCCESS;
	uint32_t           i        = 0;
	uint32_t           found    = 0;
	char               eid[TEE_OBJECT_ID_MAX_LEN];
	size_t             eid_len  = 0;

	/* create 10 objects */
	for (i = 0; i < W77Q_FS_TEST_ENUM_CNT; i++) {
		TEE_MemMove(id, prefix, pfx_len);
		id[pfx_len]     = '0' + (char)(i / 10);
		id[pfx_len + 1] = '0' + (char)(i % 10);
		id[pfx_len + 2] = '\0';

		res = TEE_CreatePersistentObject(STORAGE,
						 id, pfx_len + 2,
						 FL_RW | TEE_DATA_FLAG_OVERWRITE,
						 TEE_HANDLE_NULL,
						 id, pfx_len + 2,
						 &obj);
		CHECK(res, delete_all);
		TEE_CloseObject(obj);
		obj = TEE_HANDLE_NULL;
	}

	/* enumerate and count matching objects */
	res = TEE_AllocatePersistentObjectEnumerator(&en);
	CHECK(res, delete_all);

	res = TEE_StartPersistentObjectEnumerator(en, STORAGE);
	CHECK(res, free_enum);

	while (true) {
		eid_len = sizeof(eid);
		res = TEE_GetNextPersistentObject(en, NULL, eid, &eid_len);
		if (res == TEE_ERROR_ITEM_NOT_FOUND)
			break;
		CHECK(res, free_enum);

		/* count only objects with our prefix */
		if (eid_len >= pfx_len &&
		    TEE_MemCompare(eid, prefix, pfx_len) == 0)
			found++;
	}
	res = TEE_SUCCESS;

	TEE_FreePersistentObjectEnumerator(en);
	en = TEE_HANDLE_NULL;

	PASS_IF(found == W77Q_FS_TEST_ENUM_CNT, delete_all_fail);

	/* delete all 10 */
delete_all:
	for (i = 0; i < W77Q_FS_TEST_ENUM_CNT; i++) {
		TEE_MemMove(id, prefix, pfx_len);
		id[pfx_len]     = '0' + (char)(i / 10);
		id[pfx_len + 1] = '0' + (char)(i % 10);
		id[pfx_len + 2] = '\0';
		if (TEE_OpenPersistentObject(STORAGE, id, pfx_len + 2,
					     FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
					     &obj) == TEE_SUCCESS)
			TEE_CloseAndDeletePersistentObject1(obj);
		obj = TEE_HANDLE_NULL;
	}
	return res;

delete_all_fail:
	res = TEE_ERROR_GENERIC;
	goto delete_all;

free_enum:
	if (en != TEE_HANDLE_NULL)
		TEE_FreePersistentObjectEnumerator(en);
	goto delete_all;
}

/* ---- TC 4: LARGE (8 KB payload, pattern verify) ------------------------- */
/*
 * Writes an 8 KB object where byte[i] = i % 251 (prime), reads it back in
 * full and verifies every byte.  Tests multi-page flash writes and the
 * full-flash-read path of w77q_fs_read().
 */
static TEE_Result tc_large(void)
{
	static const char  id[]  = "wft.large";
	uint8_t            *buf  = NULL;
	TEE_ObjectHandle   obj   = TEE_HANDLE_NULL;
	TEE_Result         res   = TEE_SUCCESS;
	size_t             rd    = 0;
	uint32_t           i     = 0;

	buf = TEE_Malloc(W77Q_FS_TEST_LARGE_SZ, TEE_MALLOC_FILL_ZERO);
	if (!buf)
		return TEE_ERROR_OUT_OF_MEMORY;

	/* fill with pattern i % 251 */
	for (i = 0; i < W77Q_FS_TEST_LARGE_SZ; i++)
		buf[i] = (uint8_t)(i % 251U);

	/* create */
	res = TEE_CreatePersistentObject(STORAGE,
					 id, sizeof(id) - 1,
					 FL_RW | TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 buf, W77Q_FS_TEST_LARGE_SZ,
					 &obj);
	CHECK(res, out);
	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* open + read */
	res = TEE_OpenPersistentObject(STORAGE,
				       id, sizeof(id) - 1,
				       FL_RD, &obj);
	CHECK(res, cleanup);

	TEE_MemFill(buf, 0, W77Q_FS_TEST_LARGE_SZ);
	res = TEE_ReadObjectData(obj, buf, W77Q_FS_TEST_LARGE_SZ, &rd);
	CHECK(res, cleanup);
	PASS_IF(rd == W77Q_FS_TEST_LARGE_SZ, cleanup_fail);

	for (i = 0; i < W77Q_FS_TEST_LARGE_SZ; i++) {
		if (buf[i] != (uint8_t)(i % 251U)) {
			EMSG("large: mismatch at byte %u: got 0x%02x exp 0x%02x",
			     i, buf[i], (uint8_t)(i % 251U));
			goto cleanup_fail;
		}
	}

	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* delete */
	res = TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				       FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				       &obj);
	CHECK(res, out);
	res = TEE_CloseAndDeletePersistentObject1(obj);
	obj = TEE_HANDLE_NULL;
	goto out;

cleanup_fail:
	res = TEE_ERROR_GENERIC;
cleanup:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	if (TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				     FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				     &obj) == TEE_SUCCESS)
		TEE_CloseAndDeletePersistentObject1(obj);
out:
	TEE_Free(buf);
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	return res;
}

/* ---- TC 5: OVERWRITE (create-with-overwrite replaces existing data) ----- */
/*
 * Creates "wft.ovr" with 32 bytes of 'A', then creates it again using
 * TEE_DATA_FLAG_OVERWRITE with 32 bytes of 'B'.  Reads back and verifies
 * the content is 'B'.
 * Exercises: the overwrite branch in w77q_fs_create().
 */
static TEE_Result tc_overwrite(void)
{
	static const char  id[]  = "wft.ovr";
	TEE_ObjectHandle   obj   = TEE_HANDLE_NULL;
	TEE_Result         res   = TEE_SUCCESS;
	char               buf[W77Q_FS_TEST_BASIC_SZ];
	uint8_t            dataA[W77Q_FS_TEST_BASIC_SZ];
	uint8_t            dataB[W77Q_FS_TEST_BASIC_SZ];
	size_t             rd    = 0;
	uint32_t           i     = 0;

	for (i = 0; i < W77Q_FS_TEST_BASIC_SZ; i++) {
		dataA[i] = 'A';
		dataB[i] = 'B';
	}

	/* first create with dataA */
	res = TEE_CreatePersistentObject(STORAGE,
					 id, sizeof(id) - 1,
					 FL_RW,
					 TEE_HANDLE_NULL,
					 dataA, sizeof(dataA),
					 &obj);
	CHECK(res, out);
	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* overwrite with dataB */
	res = TEE_CreatePersistentObject(STORAGE,
					 id, sizeof(id) - 1,
					 FL_RW | TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 dataB, sizeof(dataB),
					 &obj);
	CHECK(res, cleanup);
	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* verify dataB */
	res = TEE_OpenPersistentObject(STORAGE,
				       id, sizeof(id) - 1,
				       FL_RD, &obj);
	CHECK(res, cleanup);

	TEE_MemFill(buf, 0, sizeof(buf));
	res = TEE_ReadObjectData(obj, buf, sizeof(buf), &rd);
	CHECK(res, cleanup);
	PASS_IF(rd == sizeof(dataB), cleanup_fail);

	for (i = 0; i < sizeof(dataB); i++)
		PASS_IF(buf[i] == 'B', cleanup_fail);

	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* delete */
	res = TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				       FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				       &obj);
	CHECK(res, out);
	res = TEE_CloseAndDeletePersistentObject1(obj);
	obj = TEE_HANDLE_NULL;
	goto out;

cleanup_fail:
	res = TEE_ERROR_GENERIC;
cleanup:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	if (TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				     FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				     &obj) == TEE_SUCCESS)
		TEE_CloseAndDeletePersistentObject1(obj);
	return res;
out:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	return res;
}

/* ---- TC 6: TRUNCATE ----------------------------------------------------- */
/*
 * Creates "wft.trunc" with W77Q_FS_TEST_TRUNC_SZ (256) bytes, truncates it
 * to half (128 bytes), reopens and reads back — verifies 128 bytes returned
 * and the content matches the first half of the original payload.
 * Exercises: TEE_TruncateObjectData.
 */
static TEE_Result tc_truncate(void)
{
	static const char  id[]    = "wft.trunc";
	const size_t       full_sz = W77Q_FS_TEST_TRUNC_SZ;
	const size_t       half_sz = W77Q_FS_TEST_TRUNC_SZ / 2U;
	uint8_t            *data   = NULL;
	uint8_t            *rbuf   = NULL;
	TEE_ObjectHandle   obj     = TEE_HANDLE_NULL;
	TEE_Result         res     = TEE_SUCCESS;
	size_t             rd      = 0;
	uint32_t           i       = 0;

	data = TEE_Malloc(full_sz, TEE_MALLOC_FILL_ZERO);
	rbuf = TEE_Malloc(full_sz, TEE_MALLOC_FILL_ZERO);
	if (!data || !rbuf) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}

	/* fill: byte[i] = i % 199 */
	for (i = 0; i < full_sz; i++)
		data[i] = (uint8_t)(i % 199U);

	/* create with full data */
	res = TEE_CreatePersistentObject(STORAGE,
					 id, sizeof(id) - 1,
					 FL_RW | TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 data, full_sz,
					 &obj);
	CHECK(res, out);

	/* truncate to half while still open */
	res = TEE_TruncateObjectData(obj, half_sz);
	CHECK(res, cleanup);
	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* reopen + read */
	res = TEE_OpenPersistentObject(STORAGE,
				       id, sizeof(id) - 1,
				       FL_RD, &obj);
	CHECK(res, cleanup);

	res = TEE_ReadObjectData(obj, rbuf, full_sz, &rd);
	CHECK(res, cleanup);
	PASS_IF(rd == half_sz, cleanup_fail);

	for (i = 0; i < half_sz; i++) {
		if (rbuf[i] != data[i]) {
			EMSG("truncate: byte[%u] got 0x%02x exp 0x%02x",
			     i, rbuf[i], data[i]);
			goto cleanup_fail;
		}
	}

	TEE_CloseObject(obj);
	obj = TEE_HANDLE_NULL;

	/* delete */
	res = TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				       FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				       &obj);
	CHECK(res, out);
	res = TEE_CloseAndDeletePersistentObject1(obj);
	obj = TEE_HANDLE_NULL;
	goto out;

cleanup_fail:
	res = TEE_ERROR_GENERIC;
cleanup:
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	if (TEE_OpenPersistentObject(STORAGE, id, sizeof(id) - 1,
				     FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				     &obj) == TEE_SUCCESS)
		TEE_CloseAndDeletePersistentObject1(obj);
out:
	TEE_Free(data);
	TEE_Free(rbuf);
	if (obj != TEE_HANDLE_NULL)
		TEE_CloseObject(obj);
	return res;
}

/* ---- TC_DIAG_CREATE / TC_DIAG_DELETE (flash inspection helpers) ---------- */
/*
 * TC_DIAG_CREATE creates object "wft.diag" with a 32-byte payload and
 * intentionally leaves it in flash so the host can run w77q-dump between
 * create and delete to inspect the raw flash state.
 * TC_DIAG_DELETE removes it.  Together they bracket a visible flash snapshot.
 */

static const char  _DIAG_ID[]   = "wft.diag";
static const char  _DIAG_DATA[] = "w77q_fs_diag_payload_32bytes!!!";

static TEE_Result tc_diag_create(void)
{
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_Result res;

	res = TEE_CreatePersistentObject(STORAGE,
					 _DIAG_ID, sizeof(_DIAG_ID) - 1,
					 FL_RW | TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 _DIAG_DATA, sizeof(_DIAG_DATA) - 1,
					 &obj);
	if (res == TEE_SUCCESS)
		TEE_CloseObject(obj);
	return res;
}

static TEE_Result tc_diag_delete(void)
{
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_Result res;

	res = TEE_OpenPersistentObject(STORAGE,
				       _DIAG_ID, sizeof(_DIAG_ID) - 1,
				       FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
				       &obj);
	if (res != TEE_SUCCESS)
		return res;
	return TEE_CloseAndDeletePersistentObject1(obj);
}

/* ---- TC_DIAG_SETUP / TC_DIAG_TEARDOWN ----------------------------------- */
/*
 * TC_DIAG_SETUP  (cmd 10): Pre-create the initial object(s) for TC[tc_idx]
 *   and leave them in flash so the host can call w77q-dump between setup and
 *   the actual TC run.  The real TC will then OVERWRITE/delete them as normal.
 *   Returns TEE_ERROR_NOT_SUPPORTED for TC_OVERWRITE (its first create is
 *   without TEE_DATA_FLAG_OVERWRITE — pre-creating would cause CONFLICT).
 *
 * TC_DIAG_TEARDOWN (cmd 11): Best-effort delete any objects owned by TC[n].
 *   Used to clean up after a failed TC run.
 *
 * params[0] = VALUE_INPUT;  params[0].value.a = tc_idx (0-6).
 */

static TEE_Result tc_diag_setup(uint32_t tc_idx)
{
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_Result res = TEE_SUCCESS;

	switch (tc_idx) {
	case W77Q_FS_TC_BASIC: {
		static const char id[]   = "wft.basic";
		static const char data[] = "w77q_fs_test_basic_payload_32B!";
		res = TEE_CreatePersistentObject(STORAGE, id, sizeof(id) - 1,
				FL_RW | TEE_DATA_FLAG_OVERWRITE, TEE_HANDLE_NULL,
				data, sizeof(data) - 1, &obj);
		if (res == TEE_SUCCESS) TEE_CloseObject(obj);
		break;
	}
	case W77Q_FS_TC_UPDATE: {
		static const char id[]   = "wft.update";
		static const char data[] = "payload-AAAA-initial-32bytes!!!";
		res = TEE_CreatePersistentObject(STORAGE, id, sizeof(id) - 1,
				FL_RW | TEE_DATA_FLAG_OVERWRITE, TEE_HANDLE_NULL,
				data, sizeof(data) - 1, &obj);
		if (res == TEE_SUCCESS) TEE_CloseObject(obj);
		break;
	}
	case W77Q_FS_TC_RENAME: {
		static const char id[]   = "wft.ren.src";
		static const char data[] = "rename-test-payload";
		res = TEE_CreatePersistentObject(STORAGE, id, sizeof(id) - 1,
				FL_RW | TEE_DATA_FLAG_OVERWRITE, TEE_HANDLE_NULL,
				data, sizeof(data) - 1, &obj);
		if (res == TEE_SUCCESS) TEE_CloseObject(obj);
		break;
	}
	case W77Q_FS_TC_ENUM: {
		static const char prefix[] = "wft.enum.";
		const size_t pfx_len = sizeof(prefix) - 1;
		char id[16];
		uint32_t i;

		for (i = 0; i < W77Q_FS_TEST_ENUM_CNT; i++) {
			TEE_MemMove(id, prefix, pfx_len);
			id[pfx_len]     = '0' + (char)(i / 10);
			id[pfx_len + 1] = '0' + (char)(i % 10);
			id[pfx_len + 2] = '\0';
			res = TEE_CreatePersistentObject(STORAGE, id, pfx_len + 2,
					FL_RW | TEE_DATA_FLAG_OVERWRITE,
					TEE_HANDLE_NULL, id, pfx_len + 2, &obj);
			if (res != TEE_SUCCESS)
				return res;
			TEE_CloseObject(obj);
			obj = TEE_HANDLE_NULL;
		}
		break;
	}
	case W77Q_FS_TC_LARGE: {
		static const char id[] = "wft.large";
		uint8_t *buf = TEE_Malloc(W77Q_FS_TEST_LARGE_SZ, TEE_MALLOC_FILL_ZERO);
		uint32_t i;

		if (!buf) return TEE_ERROR_OUT_OF_MEMORY;
		for (i = 0; i < W77Q_FS_TEST_LARGE_SZ; i++)
			buf[i] = (uint8_t)(i % 251U);
		res = TEE_CreatePersistentObject(STORAGE, id, sizeof(id) - 1,
				FL_RW | TEE_DATA_FLAG_OVERWRITE, TEE_HANDLE_NULL,
				buf, W77Q_FS_TEST_LARGE_SZ, &obj);
		TEE_Free(buf);
		if (res == TEE_SUCCESS) TEE_CloseObject(obj);
		break;
	}
	case W77Q_FS_TC_OVERWRITE:
		/* TC_OVERWRITE's first create is WITHOUT OVERWRITE — pre-creating
		 * would cause TEE_ERROR_ACCESS_CONFLICT.  Signal caller to skip. */
		return TEE_ERROR_NOT_SUPPORTED;

	case W77Q_FS_TC_TRUNCATE: {
		static const char id[] = "wft.trunc";
		uint8_t *buf = TEE_Malloc(W77Q_FS_TEST_TRUNC_SZ, TEE_MALLOC_FILL_ZERO);
		uint32_t i;

		if (!buf) return TEE_ERROR_OUT_OF_MEMORY;
		for (i = 0; i < W77Q_FS_TEST_TRUNC_SZ; i++)
			buf[i] = (uint8_t)(i % 251U);
		res = TEE_CreatePersistentObject(STORAGE, id, sizeof(id) - 1,
				FL_RW | TEE_DATA_FLAG_OVERWRITE, TEE_HANDLE_NULL,
				buf, W77Q_FS_TEST_TRUNC_SZ, &obj);
		TEE_Free(buf);
		if (res == TEE_SUCCESS) TEE_CloseObject(obj);
		break;
	}
	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}
	return res;
}

static TEE_Result tc_diag_teardown(uint32_t tc_idx)
{
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;

#define _DEL(id_str) \
	do { \
		if (TEE_OpenPersistentObject(STORAGE, id_str, sizeof(id_str) - 1, \
				FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META, &obj) \
				== TEE_SUCCESS) { \
			TEE_CloseAndDeletePersistentObject1(obj); \
			obj = TEE_HANDLE_NULL; \
		} \
	} while (0)

	switch (tc_idx) {
	case W77Q_FS_TC_BASIC:     _DEL("wft.basic");   break;
	case W77Q_FS_TC_UPDATE:    _DEL("wft.update");  break;
	case W77Q_FS_TC_RENAME:    _DEL("wft.ren.src"); _DEL("wft.ren.dst"); break;
	case W77Q_FS_TC_ENUM: {
		static const char prefix[] = "wft.enum.";
		const size_t pfx_len = sizeof(prefix) - 1;
		char id[16];
		uint32_t i;

		for (i = 0; i < W77Q_FS_TEST_ENUM_CNT; i++) {
			TEE_MemMove(id, prefix, pfx_len);
			id[pfx_len]     = '0' + (char)(i / 10);
			id[pfx_len + 1] = '0' + (char)(i % 10);
			id[pfx_len + 2] = '\0';
			if (TEE_OpenPersistentObject(STORAGE, id, pfx_len + 2,
					FL_WR | TEE_DATA_FLAG_ACCESS_WRITE_META,
					&obj) == TEE_SUCCESS) {
				TEE_CloseAndDeletePersistentObject1(obj);
				obj = TEE_HANDLE_NULL;
			}
		}
		break;
	}
	case W77Q_FS_TC_LARGE:     _DEL("wft.large");   break;
	case W77Q_FS_TC_OVERWRITE: _DEL("wft.ovr");     break;
	case W77Q_FS_TC_TRUNCATE:  _DEL("wft.trunc");   break;
	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}
#undef _DEL
	return TEE_SUCCESS;
}

/* ---- TC_ALL dispatcher -------------------------------------------------- */

typedef TEE_Result (*tc_fn)(void);

static const tc_fn tc_table[W77Q_FS_TC_COUNT] = {
	tc_basic,
	tc_update,
	tc_rename,
	tc_enum,
	tc_large,
	tc_overwrite,
	tc_truncate,
};

static TEE_Result tc_all(uint32_t *fail_mask)
{
	uint32_t mask = 0;
	uint32_t i    = 0;
	TEE_Result res = TEE_SUCCESS;

	for (i = 0; i < W77Q_FS_TC_COUNT; i++) {
		res = tc_table[i]();
		if (res != TEE_SUCCESS) {
			EMSG("TC %u FAILED: 0x%08x", i, res);
			mask |= (1U << i);
		}
	}
	*fail_mask = mask;
	return mask ? TEE_ERROR_GENERIC : TEE_SUCCESS;
}

/* ---- OP-TEE TA entry points --------------------------------------------- */

TEE_Result TA_CreateEntryPoint(void) { return TEE_SUCCESS; }
void       TA_DestroyEntryPoint(void) {}
TEE_Result TA_OpenSessionEntryPoint(uint32_t pt __unused,
				    TEE_Param p[4] __unused,
				    void **sess_ctx __unused)
{
	return TEE_SUCCESS;
}
void TA_CloseSessionEntryPoint(void *sess_ctx __unused) {}

TEE_Result TA_InvokeCommandEntryPoint(void *sess_ctx __unused,
				      uint32_t cmd,
				      uint32_t pt,
				      TEE_Param p[4])
{
	(void)pt;

	switch (cmd) {
	case W77Q_FS_TC_BASIC:     return tc_basic();
	case W77Q_FS_TC_UPDATE:    return tc_update();
	case W77Q_FS_TC_RENAME:    return tc_rename();
	case W77Q_FS_TC_ENUM:      return tc_enum();
	case W77Q_FS_TC_LARGE:     return tc_large();
	case W77Q_FS_TC_OVERWRITE: return tc_overwrite();
	case W77Q_FS_TC_TRUNCATE:  return tc_truncate();
	case W77Q_FS_TC_ALL: {
		uint32_t mask = 0;
		TEE_Result res = tc_all(&mask);

		if (TEE_PARAM_TYPE_GET(pt, 0) == TEE_PARAM_TYPE_VALUE_OUTPUT)
			p[0].value.a = mask;
		return res;
	}
	case W77Q_FS_TC_DIAG_CREATE: return tc_diag_create();
	case W77Q_FS_TC_DIAG_DELETE: return tc_diag_delete();
	case W77Q_FS_TC_DIAG_SETUP:
	case W77Q_FS_TC_DIAG_TEARDOWN: {
		uint32_t tc_idx = 0;

		if (TEE_PARAM_TYPE_GET(pt, 0) != TEE_PARAM_TYPE_VALUE_INPUT)
			return TEE_ERROR_BAD_PARAMETERS;
		tc_idx = p[0].value.a;
		return (cmd == W77Q_FS_TC_DIAG_SETUP) ? tc_diag_setup(tc_idx)
						       : tc_diag_teardown(tc_idx);
	}
	default:
		return TEE_ERROR_NOT_SUPPORTED;
	}
}
