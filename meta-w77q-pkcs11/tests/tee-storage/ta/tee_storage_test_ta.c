// SPDX-License-Identifier: BSD-2-Clause
/*
 * tee_storage_test_ta.c — Generic TEE secure storage test Trusted Application.
 *
 * Implements a simple key→value store over TEE_STORAGE_PRIVATE so a Python
 * host app can exercise every GP TEE storage operation without a compiled
 * C host binary.  Each object ID is the raw key bytes supplied by the host.
 *
 * Commands: WRITE, READ, DELETE, EXISTS, LIST, GET_SIZE, CLEAR_ALL.
 * See tee_storage_test_ta.h for parameter conventions.
 */

#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <string.h>

#include "tee_storage_test_ta.h"

#define STORAGE  TEE_STORAGE_PRIVATE
#define FL_RW   (TEE_DATA_FLAG_ACCESS_READ  | TEE_DATA_FLAG_ACCESS_WRITE)
#define FL_META  TEE_DATA_FLAG_ACCESS_WRITE_META

/* ---- helpers ------------------------------------------------------------- */

#define RET_IF_FAIL(res, label) \
	do { if ((res) != TEE_SUCCESS) goto label; } while (0)

/*
 * GP storage functions require the objectID pointer to reside in TA-private
 * memory (not in a shared param buffer). The OP-TEE kernel copies objectID
 * via bb_memdup_user_private() which uses TEE_MEMORY_ACCESS_READ without the
 * ANY_OWNER flag, so it rejects param-mapped pages with ACCESS_DENIED.
 * TEE_OpenPersistentObject/TEE_CreatePersistentObject treat ACCESS_DENIED as
 * an unexpected error and call TEE_Panic(). Always copy the key from the param
 * buffer into a local stack array before calling storage functions.
 */
#define COPY_OBJ_ID(dst, p, idx) \
	TEE_MemMove((dst), (p)[(idx)].memref.buffer, (p)[(idx)].memref.size)

/* ---- CMD_WRITE ----------------------------------------------------------- */

static TEE_Result cmd_write(uint32_t pt, TEE_Param p[4])
{
	uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
				       TEE_PARAM_TYPE_MEMREF_INPUT,
				       TEE_PARAM_TYPE_MEMREF_INPUT,
				       TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	char obj_id[TST_MAX_KEY_LEN];
	TEE_Result res;

	if (pt != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	if (p[1].memref.size == 0 || p[1].memref.size > TST_MAX_KEY_LEN)
		return TEE_ERROR_BAD_PARAMETERS;

	COPY_OBJ_ID(obj_id, p, 1);
	res = TEE_CreatePersistentObject(STORAGE,
					 obj_id, p[1].memref.size,
					 FL_RW | TEE_DATA_FLAG_OVERWRITE,
					 TEE_HANDLE_NULL,
					 p[2].memref.buffer, p[2].memref.size,
					 &obj);
	if (res == TEE_SUCCESS)
		TEE_CloseObject(obj);
	return res;
}

/* ---- CMD_READ ------------------------------------------------------------ */

static TEE_Result cmd_read(uint32_t pt, TEE_Param p[4])
{
	uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
				       TEE_PARAM_TYPE_MEMREF_INPUT,
				       TEE_PARAM_TYPE_MEMREF_OUTPUT,
				       TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_ObjectInfo   info;
	char             obj_id[TST_MAX_KEY_LEN];
	TEE_Result       res;
	uint32_t         rd  = 0;

	if (pt != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	if (p[1].memref.size == 0 || p[1].memref.size > TST_MAX_KEY_LEN)
		return TEE_ERROR_BAD_PARAMETERS;

	COPY_OBJ_ID(obj_id, p, 1);
	res = TEE_OpenPersistentObject(STORAGE,
				       obj_id, p[1].memref.size,
				       TEE_DATA_FLAG_ACCESS_READ, &obj);
	RET_IF_FAIL(res, out);

	res = TEE_GetObjectInfo1(obj, &info);
	RET_IF_FAIL(res, out_close);

	p[0].value.a = (uint32_t)info.dataSize;

	if (p[2].memref.size < info.dataSize) {
		res = TEE_ERROR_SHORT_BUFFER;
		goto out_close;
	}

	res = TEE_ReadObjectData(obj, p[2].memref.buffer,
				 p[2].memref.size, &rd);
	if (res == TEE_SUCCESS)
		p[0].value.a = rd;

out_close:
	TEE_CloseObject(obj);
out:
	return res;
}

/* ---- CMD_DELETE ---------------------------------------------------------- */

static TEE_Result cmd_delete(uint32_t pt, TEE_Param p[4])
{
	uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
				       TEE_PARAM_TYPE_MEMREF_INPUT,
				       TEE_PARAM_TYPE_NONE,
				       TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	char             obj_id[TST_MAX_KEY_LEN];
	TEE_Result       res;

	if (pt != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	if (p[1].memref.size == 0 || p[1].memref.size > TST_MAX_KEY_LEN)
		return TEE_ERROR_BAD_PARAMETERS;

	COPY_OBJ_ID(obj_id, p, 1);
	res = TEE_OpenPersistentObject(STORAGE,
				       obj_id, p[1].memref.size,
				       FL_RW | FL_META, &obj);
	if (res != TEE_SUCCESS)
		return res;
	return TEE_CloseAndDeletePersistentObject1(obj);
}

/* ---- CMD_EXISTS ---------------------------------------------------------- */

static TEE_Result cmd_exists(uint32_t pt, TEE_Param p[4])
{
	uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
				       TEE_PARAM_TYPE_MEMREF_INPUT,
				       TEE_PARAM_TYPE_NONE,
				       TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_ObjectInfo   info;
	char             obj_id[TST_MAX_KEY_LEN];
	TEE_Result       res;

	if (pt != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	if (p[1].memref.size == 0 || p[1].memref.size > TST_MAX_KEY_LEN)
		return TEE_ERROR_BAD_PARAMETERS;

	COPY_OBJ_ID(obj_id, p, 1);
	res = TEE_OpenPersistentObject(STORAGE,
				       obj_id, p[1].memref.size,
				       TEE_DATA_FLAG_ACCESS_READ, &obj);
	if (res == TEE_SUCCESS) {
		p[0].value.a = 1;
		if (TEE_GetObjectInfo1(obj, &info) == TEE_SUCCESS)
			p[0].value.b = (uint32_t)info.dataSize;
		TEE_CloseObject(obj);
	} else if (res == TEE_ERROR_ITEM_NOT_FOUND) {
		p[0].value.a = 0;
		p[0].value.b = 0;
		res = TEE_SUCCESS;
	}
	return res;
}

/* ---- CMD_LIST ------------------------------------------------------------ */

static TEE_Result cmd_list(uint32_t pt, TEE_Param p[4])
{
	uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
				       TEE_PARAM_TYPE_MEMREF_OUTPUT,
				       TEE_PARAM_TYPE_NONE,
				       TEE_PARAM_TYPE_NONE);
	TEE_ObjectEnumHandle en  = TEE_HANDLE_NULL;
	TEE_Result           res;
	uint32_t             cnt = 0;
	char                *buf = (char *)p[1].memref.buffer;
	size_t               cap = p[1].memref.size;
	size_t               pos = 0;
	char                 id[TEE_OBJECT_ID_MAX_LEN];
	size_t               id_len;

	if (pt != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	res = TEE_AllocatePersistentObjectEnumerator(&en);
	RET_IF_FAIL(res, out);

	res = TEE_StartPersistentObjectEnumerator(en, STORAGE);
	if (res == TEE_ERROR_ITEM_NOT_FOUND) {
		/* storage is empty — not an error */
		res = TEE_SUCCESS;
		goto out_free;
	}
	RET_IF_FAIL(res, out_free);

	while (1) {
		id_len = sizeof(id);
		res = TEE_GetNextPersistentObject(en, NULL, id, &id_len);
		if (res == TEE_ERROR_ITEM_NOT_FOUND) {
			res = TEE_SUCCESS;
			break;
		}
		RET_IF_FAIL(res, out_free);

		cnt++;
		/* append NUL-terminated ID to output buffer if space */
		if (buf && pos + id_len + 1 <= cap) {
			TEE_MemMove(buf + pos, id, id_len);
			buf[pos + id_len] = '\0';
			pos += id_len + 1;
		}
	}

out_free:
	TEE_FreePersistentObjectEnumerator(en);
out:
	p[0].value.a = cnt;
	return res;
}

/* ---- CMD_GET_SIZE -------------------------------------------------------- */

static TEE_Result cmd_get_size(uint32_t pt, TEE_Param p[4])
{
	uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
				       TEE_PARAM_TYPE_MEMREF_INPUT,
				       TEE_PARAM_TYPE_NONE,
				       TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_ObjectInfo   info;
	char             obj_id[TST_MAX_KEY_LEN];
	TEE_Result       res;

	if (pt != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	if (p[1].memref.size == 0 || p[1].memref.size > TST_MAX_KEY_LEN)
		return TEE_ERROR_BAD_PARAMETERS;

	COPY_OBJ_ID(obj_id, p, 1);
	res = TEE_OpenPersistentObject(STORAGE,
				       obj_id, p[1].memref.size,
				       TEE_DATA_FLAG_ACCESS_READ, &obj);
	if (res != TEE_SUCCESS)
		return res;

	res = TEE_GetObjectInfo1(obj, &info);
	if (res == TEE_SUCCESS)
		p[0].value.a = (uint32_t)info.dataSize;
	TEE_CloseObject(obj);
	return res;
}

/* ---- CMD_CLEAR_ALL ------------------------------------------------------- */

static TEE_Result cmd_clear_all(uint32_t pt, TEE_Param p[4])
{
	uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
				       TEE_PARAM_TYPE_NONE,
				       TEE_PARAM_TYPE_NONE,
				       TEE_PARAM_TYPE_NONE);
	TEE_ObjectEnumHandle en  = TEE_HANDLE_NULL;
	TEE_ObjectHandle     obj = TEE_HANDLE_NULL;
	TEE_Result           res;
	uint32_t             cnt = 0;
	char                 id[TEE_OBJECT_ID_MAX_LEN];
	size_t               id_len;

	if (pt != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	/*
	 * Enumeration + deletion in a single pass is unsafe because deleting
	 * invalidates the enumerator position.  Use a simple restart-loop:
	 * each pass finds the first object and deletes it.
	 */
	while (1) {
		res = TEE_AllocatePersistentObjectEnumerator(&en);
		if (res != TEE_SUCCESS)
			break;

		res = TEE_StartPersistentObjectEnumerator(en, STORAGE);
		if (res == TEE_ERROR_ITEM_NOT_FOUND) {
			TEE_FreePersistentObjectEnumerator(en);
			en  = TEE_HANDLE_NULL;
			res = TEE_SUCCESS;
			break;
		}
		if (res != TEE_SUCCESS) {
			TEE_FreePersistentObjectEnumerator(en);
			break;
		}

		id_len = sizeof(id);
		res = TEE_GetNextPersistentObject(en, NULL, id, &id_len);
		TEE_FreePersistentObjectEnumerator(en);
		en = TEE_HANDLE_NULL;

		if (res == TEE_ERROR_ITEM_NOT_FOUND) {
			res = TEE_SUCCESS;
			break;
		}
		if (res != TEE_SUCCESS)
			break;

		res = TEE_OpenPersistentObject(STORAGE, id, id_len,
					       FL_RW | FL_META, &obj);
		if (res == TEE_SUCCESS) {
			TEE_CloseAndDeletePersistentObject1(obj);
			obj = TEE_HANDLE_NULL;
			cnt++;
		}
	}

	p[0].value.a = cnt;
	return res;
}

/* ---- OP-TEE entry points ------------------------------------------------- */

TEE_Result TA_CreateEntryPoint(void)  { return TEE_SUCCESS; }
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
	switch (cmd) {
	case TST_CMD_WRITE:     return cmd_write(pt, p);
	case TST_CMD_READ:      return cmd_read(pt, p);
	case TST_CMD_DELETE:    return cmd_delete(pt, p);
	case TST_CMD_EXISTS:    return cmd_exists(pt, p);
	case TST_CMD_LIST:      return cmd_list(pt, p);
	case TST_CMD_GET_SIZE:  return cmd_get_size(pt, p);
	case TST_CMD_CLEAR_ALL: return cmd_clear_all(pt, p);
	default:
		return TEE_ERROR_NOT_SUPPORTED;
	}
}
