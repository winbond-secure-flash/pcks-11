// SPDX-License-Identifier: BSD-2-Clause
/*
 * tee_demo_ta.c — W77Q secure-storage demo Trusted Application.
 *
 * Demonstrates OP-TEE persistent storage over the W77Q QLIB-backed flash:
 *
 *  SET_PASSWORD  Store a master password (hashed with SHA-256).
 *  INIT          Create N demo objects in TEE_STORAGE_PRIVATE.
 *  LIST          Enumerate stored object IDs via the object enumerator.
 *  READ          Read + verify all objects; increment boot_count (power-safe).
 *  UPDATE        Power-safe update: write to .tmp → delete old → rename .tmp.
 *
 * Storage path (all layers transparent to this TA):
 *   TEE_STORAGE_PRIVATE  →  OP-TEE tee_fs  →  w77q_read/write/erase_sector
 *     →  QLIB (Secure World)  →  W77Q hardware (password-authenticated section)
 */

#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <string.h>

#include "tee_demo_ta.h"

/* -------------------------------------------------------------------------
 * Internal object ID strings
 * ---------------------------------------------------------------------- */

#define PASSWD_OBJ_ID      "w77q_demo_passwd"
#define PASSWD_OBJ_ID_LEN  16U
#define OBJ_ID_PREFIX      "w77q_demo_"
#define OBJ_ID_PREFIX_LEN  10U
#define OBJ_ID_TMPSFX      ".tmp"

/* Monotonic counter object IDs */
#define MC_OBJ_ID          "w77q_mc"
#define MC_OBJ_ID_LEN      7U

/* SHA-256 output size */
#define SHA256_SIZE        32U

/* -------------------------------------------------------------------------
 * Helper: build object ID for index N  →  "w77q_demo_NN"
 * ---------------------------------------------------------------------- */

static void make_obj_id(uint32_t idx, char *buf, size_t bufsz)
{
	(void)bufsz;
	TEE_MemMove(buf, OBJ_ID_PREFIX, OBJ_ID_PREFIX_LEN);
	buf[OBJ_ID_PREFIX_LEN]     = '0' + (char)(idx / 10);
	buf[OBJ_ID_PREFIX_LEN + 1] = '0' + (char)(idx % 10);
	buf[OBJ_ID_PREFIX_LEN + 2] = '\0';
}

static void make_tmp_id(uint32_t idx, char *buf, size_t bufsz)
{
	(void)bufsz;
	make_obj_id(idx, buf, bufsz);
	/* Append ".tmp" */
	buf[OBJ_ID_PREFIX_LEN + 2] = '.';
	buf[OBJ_ID_PREFIX_LEN + 3] = 't';
	buf[OBJ_ID_PREFIX_LEN + 4] = 'm';
	buf[OBJ_ID_PREFIX_LEN + 5] = 'p';
	buf[OBJ_ID_PREFIX_LEN + 6] = '\0';
}

/* -------------------------------------------------------------------------
 * Helper: compute checksum over bytes [4..23] of demo_object
 * Covers: index, counter, boot_count, value — excludes magic[0..3] and
 * checksum+pad[24..31].  sizeof(demo_object)=32; bound: 32-8=24 → [4..23]
 * ---------------------------------------------------------------------- */

static uint8_t obj_checksum(const struct demo_object *o)
{
	const uint8_t *p = (const uint8_t *)o;
	size_t i = 0;
	uint8_t cs = 0;

	/* Skip magic (bytes 0-3) and checksum+pad (last 8 bytes) */
	for (i = 4; i < sizeof(*o) - 8; i++)
		cs ^= p[i];
	return cs;
}

/* -------------------------------------------------------------------------
 * Helper: SHA-256 hash of password bytes
 * ---------------------------------------------------------------------- */

static TEE_Result hash_password(const void *pwd, size_t pwd_len,
				uint8_t digest[SHA256_SIZE])
{
	TEE_OperationHandle op = TEE_HANDLE_NULL;
	TEE_Result res = TEE_SUCCESS;
	size_t digest_len = SHA256_SIZE;

	res = TEE_AllocateOperation(&op, TEE_ALG_SHA256, TEE_MODE_DIGEST, 0);
	if (res)
		return res;

	TEE_DigestUpdate(op, pwd, pwd_len);
	res = TEE_DigestDoFinal(op, NULL, 0, digest, &digest_len);
	TEE_FreeOperation(op);
	return res;
}

/* -------------------------------------------------------------------------
 * Helper: constant-time byte-array comparison (avoids timing oracle)
 * ---------------------------------------------------------------------- */

static bool ct_memeq(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t diff = 0;
	size_t i = 0;

	for (i = 0; i < n; i++)
		diff |= a[i] ^ b[i];
	return diff == 0;
}

/* -------------------------------------------------------------------------
 * Helper: verify password against stored SHA-256 hash
 * Returns TEE_SUCCESS or TEE_ERROR_ACCESS_DENIED
 * ---------------------------------------------------------------------- */

static TEE_Result verify_password(const void *pwd, size_t pwd_len)
{
	TEE_Result        res     = TEE_SUCCESS;
	TEE_ObjectHandle  obj     = TEE_HANDLE_NULL;
	uint8_t           stored[SHA256_SIZE] = { };
	uint8_t           given[SHA256_SIZE]  = { };
	size_t          read_bytes = 0;

	res = TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
				       PASSWD_OBJ_ID, PASSWD_OBJ_ID_LEN,
				       TEE_DATA_FLAG_ACCESS_READ,
				       &obj);
	if (res == TEE_ERROR_ITEM_NOT_FOUND) {
		EMSG("demo_ta: no password set — call SET_PASSWORD first");
		return TEE_ERROR_ACCESS_DENIED;
	}
	if (res)
		return res;

	res = TEE_ReadObjectData(obj, stored, SHA256_SIZE, &read_bytes);
	TEE_CloseObject(obj);
	if (res || read_bytes != SHA256_SIZE)
		return TEE_ERROR_GENERIC;

	res = hash_password(pwd, pwd_len, given);
	if (res)
		return res;

	if (!ct_memeq(stored, given, SHA256_SIZE)) {
		EMSG("demo_ta: wrong password");
		return TEE_ERROR_ACCESS_DENIED;
	}
	return TEE_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Command: SET_PASSWORD
 * params[0] MEMREF_INPUT  password text
 * ---------------------------------------------------------------------- */

static TEE_Result cmd_set_password(uint32_t ptypes, TEE_Param params[4])
{
	const uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
					     TEE_PARAM_TYPE_NONE,
					     TEE_PARAM_TYPE_NONE,
					     TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	uint8_t          hash[SHA256_SIZE] = { };
	TEE_Result       res = TEE_SUCCESS;

	if (ptypes != exp)
		return TEE_ERROR_BAD_PARAMETERS;
	if (!params[0].memref.size || params[0].memref.size > DEMO_MAX_PASSWORD)
		return TEE_ERROR_BAD_PARAMETERS;

	res = hash_password(params[0].memref.buffer,
			    params[0].memref.size, hash);
	if (res)
		return res;

	res = TEE_CreatePersistentObject(TEE_STORAGE_PRIVATE,
					 PASSWD_OBJ_ID, PASSWD_OBJ_ID_LEN,
					 TEE_DATA_FLAG_ACCESS_READ |
					 TEE_DATA_FLAG_ACCESS_WRITE |
					 TEE_DATA_FLAG_ACCESS_WRITE_META |
					 TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 hash, SHA256_SIZE, &obj);
	if (!res)
		TEE_CloseObject(obj);

	IMSG("demo_ta: password %s", res ? "NOT set" : "set");
	return res;
}

/* -------------------------------------------------------------------------
 * Command: INIT
 * params[0] MEMREF_INPUT  password
 * params[1] VALUE_INPUT   a = count
 * ---------------------------------------------------------------------- */

static TEE_Result cmd_init(uint32_t ptypes, TEE_Param params[4])
{
	const uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
					     TEE_PARAM_TYPE_VALUE_INPUT,
					     TEE_PARAM_TYPE_NONE,
					     TEE_PARAM_TYPE_NONE);
	TEE_Result       res   = TEE_SUCCESS;
	TEE_ObjectHandle obj   = TEE_HANDLE_NULL;
	uint32_t         count = 0;
	uint32_t         i     = 0;
	char             id[32] = { };

	if (ptypes != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	res = verify_password(params[0].memref.buffer,
			      params[0].memref.size);
	if (res)
		return res;

	count = params[1].value.a;
	if (!count || count > DEMO_MAX_OBJECTS)
		return TEE_ERROR_BAD_PARAMETERS;

	for (i = 0; i < count; i++) {
		struct demo_object data = { };

		make_obj_id(i, id, sizeof(id));

		data.magic      = DEMO_DATA_MAGIC;
		data.index      = i;
		data.counter    = 0;
		data.boot_count = 0;
		data.value      = (uint64_t)(i + 1) * 1000ULL;
		data.checksum   = obj_checksum(&data);

		res = TEE_CreatePersistentObject(
				TEE_STORAGE_PRIVATE,
				id, OBJ_ID_PREFIX_LEN + 2,
				TEE_DATA_FLAG_ACCESS_READ |
				TEE_DATA_FLAG_ACCESS_WRITE |
				TEE_DATA_FLAG_ACCESS_WRITE_META,
				TEE_HANDLE_NULL,
				&data, sizeof(data), &obj);
		if (res == TEE_ERROR_ACCESS_CONFLICT) {
			EMSG("demo_ta: object %s already exists — call LIST/READ first",
			     id);
			return TEE_ERROR_ACCESS_CONFLICT;
		}
		if (res) {
			EMSG("demo_ta: create %s failed: %#x", id, res);
			return res;
		}
		TEE_CloseObject(obj);
		IMSG("demo_ta: created %s  value=%" PRIu64, id, data.value);
	}
	return TEE_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Command: LIST
 * params[0] MEMREF_INPUT   password
 * params[1] MEMREF_OUTPUT  buffer for object ID strings
 * params[2] VALUE_OUTPUT   a = count found
 * ---------------------------------------------------------------------- */

static TEE_Result cmd_list(uint32_t ptypes, TEE_Param params[4])
{
	const uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
					     TEE_PARAM_TYPE_MEMREF_OUTPUT,
					     TEE_PARAM_TYPE_VALUE_OUTPUT,
					     TEE_PARAM_TYPE_NONE);
	TEE_ObjectEnumHandle enumerator = TEE_HANDLE_NULL;
	TEE_Result           res        = TEE_SUCCESS;
	char  *out     = (char *)params[1].memref.buffer;
	size_t out_max = params[1].memref.size;
	uint32_t count  = 0;
	char     obj_id[TEE_OBJECT_ID_MAX_LEN + 1] = { };
	size_t   obj_id_len = 0;

	if (ptypes != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	res = verify_password(params[0].memref.buffer,
			      params[0].memref.size);
	if (res)
		return res;

	res = TEE_AllocatePersistentObjectEnumerator(&enumerator);
	if (res)
		return res;

	res = TEE_StartPersistentObjectEnumerator(enumerator,
						   TEE_STORAGE_PRIVATE);
	if (res) {
		TEE_FreePersistentObjectEnumerator(enumerator);
		if (res == TEE_ERROR_ITEM_NOT_FOUND) {
			params[2].value.a = 0;
			return TEE_SUCCESS;
		}
		return res;
	}

	while (1) {
		obj_id_len = sizeof(obj_id) - 1;
		res = TEE_GetNextPersistentObject(enumerator, NULL,
						  obj_id, &obj_id_len);
		if (res == TEE_ERROR_ITEM_NOT_FOUND)
			break;
		if (res) {
			TEE_FreePersistentObjectEnumerator(enumerator);
			return res;
		}
		obj_id[obj_id_len] = '\0';

		/* Skip the password object and .tmp objects */
		if (TEE_MemCompare(obj_id, PASSWD_OBJ_ID,
				   PASSWD_OBJ_ID_LEN) == 0)
			continue;
		if (obj_id_len > 4 &&
		    TEE_MemCompare(obj_id + obj_id_len - 4, OBJ_ID_TMPSFX,
				   4) == 0)
			continue;

		/* Copy ID into output buffer (DEMO_OBJECT_ID_LEN bytes/slot) */
		if ((count + 1) * DEMO_OBJECT_ID_LEN <= out_max) {
			char *slot = out + count * DEMO_OBJECT_ID_LEN;

			TEE_MemFill(slot, 0, DEMO_OBJECT_ID_LEN);
			TEE_MemMove(slot, obj_id,
				    (obj_id_len < DEMO_OBJECT_ID_LEN - 1)
				    ? obj_id_len : DEMO_OBJECT_ID_LEN - 1);
		}
		count++;
	}

	TEE_FreePersistentObjectEnumerator(enumerator);
	params[2].value.a = count;
	IMSG("demo_ta: LIST found %u objects", count);
	return TEE_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Command: READ
 * params[0] MEMREF_INPUT   password
 * params[1] MEMREF_OUTPUT  array of struct demo_object
 * params[2] VALUE_OUTPUT   a = count
 * ---------------------------------------------------------------------- */

static TEE_Result cmd_read(uint32_t ptypes, TEE_Param params[4])
{
	const uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
					     TEE_PARAM_TYPE_MEMREF_OUTPUT,
					     TEE_PARAM_TYPE_VALUE_OUTPUT,
					     TEE_PARAM_TYPE_NONE);
	TEE_Result        res    = TEE_SUCCESS;
	struct demo_object *out  = (struct demo_object *)params[1].memref.buffer;
	uint32_t           max   = params[1].memref.size / sizeof(struct demo_object);
	uint32_t           count = 0;
	uint32_t           i     = 0;
	char               id[32]  = { };
	char               tmp[32] = { };

	if (ptypes != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	res = verify_password(params[0].memref.buffer,
			      params[0].memref.size);
	if (res)
		return res;

	for (i = 0; i < DEMO_MAX_OBJECTS; i++) {
		TEE_ObjectHandle  src  = TEE_HANDLE_NULL;
		TEE_ObjectHandle  dst  = TEE_HANDLE_NULL;
		struct demo_object data = { };
		size_t          read_bytes = 0;

		make_obj_id(i, id, sizeof(id));
		make_tmp_id(i, tmp, sizeof(tmp));

		/* Need WRITE_META to CloseAndDelete the original below */
		res = TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
					       id, OBJ_ID_PREFIX_LEN + 2,
					       TEE_DATA_FLAG_ACCESS_READ |
					       TEE_DATA_FLAG_ACCESS_WRITE_META,
					       &src);
		if (res == TEE_ERROR_ITEM_NOT_FOUND)
			continue;
		if (res) {
			EMSG("demo_ta: open %s failed: %#x", id, res);
			return res;
		}

		res = TEE_ReadObjectData(src, &data, sizeof(data), &read_bytes);
		if (res || read_bytes != sizeof(data)) {
			TEE_CloseObject(src);
			EMSG("demo_ta: read %s failed", id);
			return res ? res : TEE_ERROR_GENERIC;
		}

		/* Validate */
		if (data.magic != DEMO_DATA_MAGIC ||
		    obj_checksum(&data) != data.checksum) {
			TEE_CloseObject(src);
			EMSG("demo_ta: %s corrupted! magic=%#x cs=%#x/%#x",
			     id, data.magic, data.checksum, obj_checksum(&data));
			return TEE_ERROR_CORRUPT_OBJECT;
		}

		/* Apply boot_count increment */
		data.boot_count++;
		data.checksum = obj_checksum(&data);

		/* Power-safe write: create .tmp → delete original → rename */
		res = TEE_CreatePersistentObject(
				TEE_STORAGE_PRIVATE,
				tmp, OBJ_ID_PREFIX_LEN + 6,
				TEE_DATA_FLAG_ACCESS_READ |
				TEE_DATA_FLAG_ACCESS_WRITE |
				TEE_DATA_FLAG_ACCESS_WRITE_META |
				TEE_DATA_FLAG_OVERWRITE,
				TEE_HANDLE_NULL,
				&data, sizeof(data), &dst);
		if (res) {
			TEE_CloseObject(src);
			EMSG("demo_ta: create .tmp for %s failed: %#x", id, res);
			return res;
		}
		TEE_CloseObject(dst);

		TEE_CloseAndDeletePersistentObject1(src);

		res = TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
					       tmp, OBJ_ID_PREFIX_LEN + 6,
					       TEE_DATA_FLAG_ACCESS_WRITE_META,
					       &dst);
		if (res) {
			EMSG("demo_ta: reopen .tmp for %s failed: %#x", id, res);
			return res;
		}
		res = TEE_RenamePersistentObject(dst, id, OBJ_ID_PREFIX_LEN + 2);
		TEE_CloseObject(dst);
		if (res) {
			EMSG("demo_ta: rename .tmp→%s failed: %#x", id, res);
			return res;
		}

		IMSG("demo_ta: read %s  idx=%u ctr=%u boots=%u val=%" PRIu64,
		     id, data.index, data.counter, data.boot_count, data.value);

		if (count < max)
			out[count] = data;
		count++;
	}

	params[2].value.a = count;
	return TEE_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Command: UPDATE (power-safe)
 * params[0] MEMREF_INPUT  password
 *
 * For each object:
 *   1. Read current data
 *   2. Write updated data to "w77q_demo_NN.tmp"   (new file, complete)
 *   3. Delete "w77q_demo_NN"                       (old file gone)
 *   4. Rename "w77q_demo_NN.tmp" → "w77q_demo_NN" (atomic swap)
 *
 * If power fails between steps 2 and 4, the .tmp exists but old doesn't;
 * recovery: scan for .tmp files at next boot and complete the rename.
 * ---------------------------------------------------------------------- */

static TEE_Result cmd_update(uint32_t ptypes, TEE_Param params[4])
{
	const uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
					     TEE_PARAM_TYPE_NONE,
					     TEE_PARAM_TYPE_NONE,
					     TEE_PARAM_TYPE_NONE);
	TEE_Result   res  = TEE_SUCCESS;
	uint32_t     i    = 0;
	char         id[32]  = { };
	char         tmp[32] = { };

	if (ptypes != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	res = verify_password(params[0].memref.buffer,
			      params[0].memref.size);
	if (res)
		return res;

	for (i = 0; i < DEMO_MAX_OBJECTS; i++) {
		TEE_ObjectHandle  src  = TEE_HANDLE_NULL;
		TEE_ObjectHandle  dst  = TEE_HANDLE_NULL;
		struct demo_object data = { };
		size_t          read_bytes = 0;

		make_obj_id(i, id, sizeof(id));
		make_tmp_id(i, tmp, sizeof(tmp));

		/* Open original */
		res = TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
					       id, OBJ_ID_PREFIX_LEN + 2,
					       TEE_DATA_FLAG_ACCESS_READ |
					       TEE_DATA_FLAG_ACCESS_WRITE_META,
					       &src);
		if (res == TEE_ERROR_ITEM_NOT_FOUND)
			continue;
		if (res)
			return res;

		res = TEE_ReadObjectData(src, &data, sizeof(data), &read_bytes);
		if (res || read_bytes != sizeof(data)) {
			TEE_CloseObject(src);
			return res ? res : TEE_ERROR_GENERIC;
		}

		/* Validate before updating */
		if (data.magic != DEMO_DATA_MAGIC ||
		    obj_checksum(&data) != data.checksum) {
			TEE_CloseObject(src);
			EMSG("demo_ta: %s corrupted before update", id);
			return TEE_ERROR_CORRUPT_OBJECT;
		}

		/* Apply update */
		data.counter++;
		data.value    += (uint64_t)(data.index + 1);
		data.checksum  = obj_checksum(&data);

		/* Step 1: write to .tmp (creates new file) */
		res = TEE_CreatePersistentObject(
				TEE_STORAGE_PRIVATE,
				tmp, OBJ_ID_PREFIX_LEN + 6,
				TEE_DATA_FLAG_ACCESS_READ |
				TEE_DATA_FLAG_ACCESS_WRITE |
				TEE_DATA_FLAG_ACCESS_WRITE_META |
				TEE_DATA_FLAG_OVERWRITE,
				TEE_HANDLE_NULL,
				&data, sizeof(data), &dst);
		if (res) {
			TEE_CloseObject(src);
			EMSG("demo_ta: create tmp for %s failed: %#x", id, res);
			return res;
		}
		TEE_CloseObject(dst);

		/* Step 2: delete original */
		/* src still open with WRITE_META — use CloseAndDelete */
		TEE_CloseAndDeletePersistentObject1(src);

		/* Step 3: rename .tmp → original */
		res = TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
					       tmp, OBJ_ID_PREFIX_LEN + 6,
					       TEE_DATA_FLAG_ACCESS_WRITE_META,
					       &dst);
		if (res) {
			EMSG("demo_ta: reopen tmp for %s failed: %#x", id, res);
			return res;
		}

		res = TEE_RenamePersistentObject(dst,
						 id, OBJ_ID_PREFIX_LEN + 2);
		TEE_CloseObject(dst);
		if (res) {
			EMSG("demo_ta: rename %s → %s failed: %#x", tmp, id, res);
			return res;
		}

		IMSG("demo_ta: updated %s  ctr=%u val=%" PRIu64,
		     id, data.counter, data.value);
	}
	return TEE_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Monotonic counter helpers
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Append-only log counter
 *
 * The counter lives in a single persistent object "w77q_mc" that is
 * pre-allocated at MC_LOG_OBJ_SZ bytes (all 0xFF) at creation time.
 * Each increment appends one 16-byte record to the next empty (0xFF)
 * slot — no sector erase and no TOC flush per increment because the
 * object size never changes.
 *
 * Record layout (16 bytes):
 *   [0..3]   magic  MC_LOG_MAGIC = valid; 0xFFFFFFFF = empty (erased NOR)
 *   [4..11]  value  uint64_t counter value (host byte order)
 *   [12..15] check  XOR of preceding 12 bytes
 *
 * When all MC_LOG_SLOTS records are full, compact: delete the old object
 * and create a fresh one whose slot-0 holds the new incremented value.
 * Compaction costs 1 data-sector erase + 1 TOC flush and happens only
 * once every MC_LOG_SLOTS increments.
 *
 * Migration: if the existing "w77q_mc" object has the old 16-byte
 * demo_mc format (DEMO_MC_MAGIC), its value is preserved in the new log.
 * ---------------------------------------------------------------------- */

static uint32_t mc_log_check(const struct mc_log_record *r)
{
	const uint8_t *p = (const uint8_t *)r;
	uint32_t v = 0;
	uint32_t w = 0;
	size_t i = 0;

	for (i = 0; i < 12; i += 4) {
		memcpy(&w, p + i, 4);
		v ^= w;
	}
	return v;
}

/* Cached log state — valid while this TA instance is alive */
static bool     g_mc_loaded = false;
static uint64_t g_mc_value  = 0;
static uint32_t g_mc_next   = 0;  /* index of next empty slot */

/*
 * mc_log_create() — create or overwrite "w77q_mc" as a fresh log.
 * Slot 0 is written with initial_value; all other slots are 0xFF.
 * Because the full MC_LOG_OBJ_SZ buffer is passed to
 * TEE_CreatePersistentObject, the object is pre-allocated at its final
 * size — subsequent record writes never grow the object and never
 * trigger a w77q_fs TOC flush.
 */
static TEE_Result mc_log_create(uint64_t initial_value)
{
	uint8_t *buf = NULL;
	struct mc_log_record rec = { };
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_Result res = TEE_SUCCESS;

	buf = TEE_Malloc(MC_LOG_OBJ_SZ, 0);
	if (!buf)
		return TEE_ERROR_OUT_OF_MEMORY;
	memset(buf, 0xFF, MC_LOG_OBJ_SZ);

	rec.magic = MC_LOG_MAGIC;
	rec.value = initial_value;
	rec.check = mc_log_check(&rec);
	memcpy(buf, &rec, sizeof(rec));

	res = TEE_CreatePersistentObject(TEE_STORAGE_PRIVATE,
					 MC_OBJ_ID, MC_OBJ_ID_LEN,
					 TEE_DATA_FLAG_ACCESS_READ |
					 TEE_DATA_FLAG_ACCESS_WRITE |
					 TEE_DATA_FLAG_ACCESS_WRITE_META |
					 TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 buf, MC_LOG_OBJ_SZ,
					 &obj);
	TEE_Free(buf);
	if (res) {
		EMSG("demo_ta: mc_log_create failed %#x", res);
		return res;
	}
	TEE_CloseObject(obj);

	g_mc_value  = initial_value;
	g_mc_next   = 1;
	g_mc_loaded = true;
	return TEE_SUCCESS;
}

/*
 * mc_log_scan() — load g_mc_value / g_mc_next from flash.
 * Handles migration from the old 16-byte demo_mc format.
 * Returns TEE_ERROR_ITEM_NOT_FOUND if no "w77q_mc" object exists yet.
 */
static TEE_Result mc_log_scan(void)
{
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_ObjectInfo   info = { };
	struct mc_log_record rec = { };
	size_t rd = 0;
	uint32_t i = 0;
	TEE_Result res = TEE_SUCCESS;

	res = TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
				       MC_OBJ_ID, MC_OBJ_ID_LEN,
				       TEE_DATA_FLAG_ACCESS_READ |
				       TEE_DATA_FLAG_ACCESS_WRITE_META,
				       &obj);
	if (res)
		return res;

	TEE_GetObjectInfo1(obj, &info);

	/* Migration: old 16-byte demo_mc format? */
	if (info.dataSize == sizeof(struct demo_mc)) {
		struct demo_mc old = { };

		TEE_ReadObjectData(obj, &old, sizeof(old), &rd);
		TEE_CloseAndDeletePersistentObject1(obj);
		IMSG("demo_ta: migrating old counter format, value=%" PRIu64,
		     old.value);
		if (old.magic == DEMO_MC_MAGIC)
			return mc_log_create(old.value);
		return mc_log_create(0);
	}

	/* New log format: scan records */
	g_mc_value = 0;
	g_mc_next  = 0;

	for (i = 0; i < MC_LOG_SLOTS; i++) {
		res = TEE_ReadObjectData(obj, &rec, sizeof(rec), &rd);
		if (res || rd != sizeof(rec))
			break;
		if (rec.magic == 0xFFFFFFFFU)
			break;  /* first empty slot */
		if (rec.magic == MC_LOG_MAGIC && mc_log_check(&rec) == rec.check) {
			g_mc_value = rec.value;
			g_mc_next  = i + 1;
		}
	}

	TEE_CloseObject(obj);
	g_mc_loaded = true;
	return TEE_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Command: COUNTER_READ — read current value (no password required)
 * params[0]: VALUE_OUTPUT  a=lo32  b=hi32
 * ---------------------------------------------------------------------- */

static TEE_Result cmd_counter_read(uint32_t ptypes, TEE_Param params[4])
{
	const uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
					     TEE_PARAM_TYPE_NONE,
					     TEE_PARAM_TYPE_NONE,
					     TEE_PARAM_TYPE_NONE);
	TEE_Result res = TEE_SUCCESS;

	if (ptypes != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	if (!g_mc_loaded) {
		res = mc_log_scan();
		if (res == TEE_ERROR_ITEM_NOT_FOUND) {
			params[0].value.a = 0;
			params[0].value.b = 0;
			return TEE_SUCCESS;
		}
		if (res)
			return res;
	}

	params[0].value.a = (uint32_t)(g_mc_value & 0xFFFFFFFFU);
	params[0].value.b = (uint32_t)(g_mc_value >> 32);
	IMSG("demo_ta: counter read: %" PRIu64, g_mc_value);
	return TEE_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Command: COUNTER_INC — append-log increment (password required)
 *
 * Fast path (log not full):
 *   Open object → seek to next slot → write 16-byte record → close.
 *   Object size does not change → w77q_fs writes no TOC sector erase.
 *
 * Compaction path (every MC_LOG_SLOTS increments):
 *   mc_log_create(new_value) with OVERWRITE — erases old object in the
 *   same TOC flush that commits the new one: 1 data erase + 1 TOC erase.
 *
 * params[0]: MEMREF_INPUT  password
 * params[1]: VALUE_OUTPUT  a=new_lo32  b=new_hi32
 * ---------------------------------------------------------------------- */

static TEE_Result cmd_counter_inc(uint32_t ptypes, TEE_Param params[4])
{
	const uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
					     TEE_PARAM_TYPE_VALUE_OUTPUT,
					     TEE_PARAM_TYPE_NONE,
					     TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	struct mc_log_record rec = { };
	uint64_t new_value = 0;
	TEE_Result res = TEE_SUCCESS;

	if (ptypes != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	res = verify_password(params[0].memref.buffer,
			      params[0].memref.size);
	if (res)
		return res;

	/* Load log state if not cached from a previous call this session */
	if (!g_mc_loaded) {
		res = mc_log_scan();
		if (res == TEE_ERROR_ITEM_NOT_FOUND) {
			/* First ever increment */
			res = mc_log_create(1);
			if (res)
				return res;
			goto done;
		}
		if (res)
			return res;
	}

	new_value = g_mc_value + 1;

	if (g_mc_next >= MC_LOG_SLOTS) {
		/* Log full — compact and record new value at slot 0 */
		IMSG("demo_ta: mc_log compact at value=%" PRIu64, new_value);
		res = mc_log_create(new_value);
		if (res)
			return res;
		goto done;
	}

	/* Fast path: append record at g_mc_next (no size change, no TOC flush) */
	rec.magic = MC_LOG_MAGIC;
	rec.value = new_value;
	rec.check = mc_log_check(&rec);

	res = TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
				       MC_OBJ_ID, MC_OBJ_ID_LEN,
				       TEE_DATA_FLAG_ACCESS_WRITE,
				       &obj);
	if (res) {
		EMSG("demo_ta: counter inc: open failed %#x", res);
		return res;
	}

	res = TEE_SeekObjectData(obj,
				 (int32_t)(g_mc_next * MC_LOG_REC_SIZE),
				 TEE_DATA_SEEK_SET);
	if (!res)
		res = TEE_WriteObjectData(obj, &rec, sizeof(rec));
	TEE_CloseObject(obj);
	if (res) {
		EMSG("demo_ta: counter inc: write failed %#x", res);
		return res;
	}

	g_mc_value = new_value;
	g_mc_next++;

done:
	params[1].value.a = (uint32_t)(g_mc_value & 0xFFFFFFFFU);
	params[1].value.b = (uint32_t)(g_mc_value >> 32);
	IMSG("demo_ta: counter → %" PRIu64 "  slot %u/%u",
	     g_mc_value, g_mc_next - 1, MC_LOG_SLOTS);
	return TEE_SUCCESS;
}

/* -------------------------------------------------------------------------
 * TA entry points
 * ---------------------------------------------------------------------- */

TEE_Result TA_CreateEntryPoint(void)
{
	IMSG("demo_ta: created");
	/* Log state is loaded lazily on first counter access */
	return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
	IMSG("demo_ta: destroyed");
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t ptypes __unused,
				    TEE_Param params[4] __unused,
				    void **session_ctx __unused)
{
	IMSG("demo_ta: session opened");
	return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *session_ctx __unused)
{
	IMSG("demo_ta: session closed");
}

TEE_Result TA_InvokeCommandEntryPoint(void *session_ctx __unused,
				      uint32_t cmd,
				      uint32_t ptypes,
				      TEE_Param params[4])
{
	switch (cmd) {
	case DEMO_CMD_SET_PASSWORD:
		return cmd_set_password(ptypes, params);
	case DEMO_CMD_INIT:
		return cmd_init(ptypes, params);
	case DEMO_CMD_LIST:
		return cmd_list(ptypes, params);
	case DEMO_CMD_READ:
		return cmd_read(ptypes, params);
	case DEMO_CMD_UPDATE:
		return cmd_update(ptypes, params);
	case DEMO_CMD_COUNTER_READ:
		return cmd_counter_read(ptypes, params);
	case DEMO_CMD_COUNTER_INC:
		return cmd_counter_inc(ptypes, params);
	default:
		EMSG("demo_ta: unknown command %u", cmd);
		return TEE_ERROR_NOT_IMPLEMENTED;
	}
}
