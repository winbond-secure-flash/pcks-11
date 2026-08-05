// SPDX-License-Identifier: BSD-2-Clause
/*
 * tee-demo.c — Linux host application for the W77Q TEE demo.
 *
 * Demonstrates the full lifecycle of password-protected secure objects
 * stored in the W77Q QLIB-backed TEE secure storage:
 *
 *   tee-demo set-password <password>
 *       Store the master password in the TA.
 *
 *   tee-demo init <password> [count]
 *       Create [count] (default 5) named demo objects.
 *
 *   tee-demo list <password>
 *       Enumerate and print all stored object IDs.
 *
 *   tee-demo read <password>
 *       Read every object, verify its checksum, print values.
 *       Increments boot_count in each object (power-safe).
 *
 *   tee-demo update <password>
 *       Power-safe update: counter++, value += (index+1).
 *       Uses write-to-tmp → delete-old → rename-tmp pattern.
 *
 *   tee-demo demo <password>
 *       Full lifecycle: init → list → read → update → read.
 *       Simulates first boot + next-boot scenario.
 *
 * Security stack:
 *   libteec (NW) → OP-TEE (SW) → tee_fs → w77q driver → QLIB → W77Q HW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>

#include <tee_client_api.h>

/* TEEC_ERROR_CORRUPT_OBJECT is not in all versions of tee_client_api.h */
#ifndef TEEC_ERROR_CORRUPT_OBJECT
#define TEEC_ERROR_CORRUPT_OBJECT  0xF0100001U
#endif

/* TEE_ERROR_ACCESS_CONFLICT: object already exists in secure storage */
#ifndef TEEC_ERROR_ACCESS_CONFLICT
#define TEEC_ERROR_ACCESS_CONFLICT  0xFFFF0003U
#endif

/* Import shared header (defines UUID, commands, struct demo_object) */
#include "tee_demo_ta.h"

/* -------------------------------------------------------------------------
 * Internal utilities
 * ---------------------------------------------------------------------- */

static const TEEC_UUID g_ta_uuid = DEMO_TA_UUID;

#define CHECK(res, label, ...) \
	do { \
		if ((res) != TEEC_SUCCESS) { \
			fprintf(stderr, "ERROR: " __VA_ARGS__); \
			fprintf(stderr, "  TEEC result: 0x%08x\n", (res)); \
			goto label; \
		} \
	} while (0)

static void print_separator(void)
{
	printf("─────────────────────────────────────────────────\n");
}

static void print_object(const struct demo_object *o, uint32_t n)
{
	printf("  [%2u] idx=%-2u  counter=%-4u  boot_count=%-4u  "
	       "value=%-10" PRIu64 "  cs=0x%02x\n",
	       n, o->index, o->counter, o->boot_count, o->value, o->checksum);
}

/* -------------------------------------------------------------------------
 * Open TEEC context + session
 * ---------------------------------------------------------------------- */

static TEEC_Result open_session(TEEC_Context *ctx, TEEC_Session *sess)
{
	TEEC_Result res = TEEC_SUCCESS;
	uint32_t    err_origin = 0;

	res = TEEC_InitializeContext(NULL, ctx);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_InitializeContext failed: 0x%08x\n", res);
		return res;
	}

	res = TEEC_OpenSession(ctx, sess, &g_ta_uuid,
			       TEEC_LOGIN_PUBLIC, NULL, NULL, &err_origin);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_OpenSession failed: 0x%08x  origin: 0x%08x\n",
			res, err_origin);
		TEEC_FinalizeContext(ctx);
	}
	return res;
}

static void close_session(TEEC_Context *ctx, TEEC_Session *sess)
{
	TEEC_CloseSession(sess);
	TEEC_FinalizeContext(ctx);
}

/* -------------------------------------------------------------------------
 * set-password
 * ---------------------------------------------------------------------- */

static int do_set_password(const char *password)
{
	TEEC_Context   ctx  = { };
	TEEC_Session   sess = { };
	TEEC_Operation op   = { };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       err_origin = 0;

	res = open_session(&ctx, &sess);
	if (res)
		return 1;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_NONE, TEEC_NONE, TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)password;
	op.params[0].tmpref.size   = strlen(password);

	res = TEEC_InvokeCommand(&sess, DEMO_CMD_SET_PASSWORD, &op, &err_origin);
	close_session(&ctx, &sess);

	if (res) {
		fprintf(stderr, "set-password failed: 0x%08x\n", res);
		return 1;
	}
	printf("✓ Password stored in TEE secure storage.\n");
	return 0;
}

/* -------------------------------------------------------------------------
 * init
 * ---------------------------------------------------------------------- */

/*
 * allow_existing: when non-zero, treat TEEC_ERROR_ACCESS_CONFLICT as success
 * (objects already exist — skip init rather than aborting).
 */
static int do_init(const char *password, uint32_t count, int allow_existing)
{
	TEEC_Context   ctx  = { };
	TEEC_Session   sess = { };
	TEEC_Operation op   = { };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       err_origin = 0;

	res = open_session(&ctx, &sess);
	if (res)
		return 1;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_VALUE_INPUT,
					 TEEC_NONE, TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)password;
	op.params[0].tmpref.size   = strlen(password);
	op.params[1].value.a       = count;

	res = TEEC_InvokeCommand(&sess, DEMO_CMD_INIT, &op, &err_origin);
	close_session(&ctx, &sess);

	if (res == TEEC_ERROR_ACCESS_DENIED) {
		fprintf(stderr, "✗ Wrong password.\n");
		return 1;
	}
	if (res == TEEC_ERROR_ACCESS_CONFLICT) {
		if (allow_existing) {
			printf("  (demo objects already exist — skipping init)\n");
			return 0;
		}
		fprintf(stderr, "✗ Objects already exist — use 'list' or 'read'.\n");
		return 1;
	}
	if (res) {
		fprintf(stderr, "init failed: 0x%08x\n", res);
		return 1;
	}
	printf("✓ Created %u demo objects in TEE secure storage.\n", count);
	return 0;
}

/* -------------------------------------------------------------------------
 * list
 * ---------------------------------------------------------------------- */

static int do_list(const char *password)
{
	TEEC_Context   ctx  = { };
	TEEC_Session   sess = { };
	TEEC_Operation op   = { };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       err_origin = 0;
	/* Buffer for DEMO_MAX_OBJECTS × DEMO_OBJECT_ID_LEN */
	char           id_buf[DEMO_MAX_OBJECTS * DEMO_OBJECT_ID_LEN] = { };
	uint32_t       count = 0;
	uint32_t       i     = 0;

	res = open_session(&ctx, &sess);
	if (res)
		return 1;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_MEMREF_TEMP_OUTPUT,
					 TEEC_VALUE_OUTPUT,
					 TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)password;
	op.params[0].tmpref.size   = strlen(password);
	op.params[1].tmpref.buffer = id_buf;
	op.params[1].tmpref.size   = sizeof(id_buf);

	res = TEEC_InvokeCommand(&sess, DEMO_CMD_LIST, &op, &err_origin);
	close_session(&ctx, &sess);

	if (res == TEEC_ERROR_ACCESS_DENIED) {
		fprintf(stderr, "✗ Wrong password.\n");
		return 1;
	}
	if (res) {
		fprintf(stderr, "list failed: 0x%08x\n", res);
		return 1;
	}

	count = op.params[2].value.a;
	print_separator();
	printf("Stored objects (%u):\n", count);
	for (i = 0; i < count && i < DEMO_MAX_OBJECTS; i++) {
		const char *slot = id_buf + i * DEMO_OBJECT_ID_LEN;

		printf("  [%u] %.*s\n", i, (int)DEMO_OBJECT_ID_LEN, slot);
	}
	print_separator();
	return 0;
}

/* -------------------------------------------------------------------------
 * read
 * ---------------------------------------------------------------------- */

static int do_read(const char *password)
{
	TEEC_Context   ctx  = { };
	TEEC_Session   sess = { };
	TEEC_Operation op   = { };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       err_origin = 0;
	struct demo_object objs[DEMO_MAX_OBJECTS] = { };
	uint32_t       count = 0;
	uint32_t       i     = 0;

	res = open_session(&ctx, &sess);
	if (res)
		return 1;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_MEMREF_TEMP_OUTPUT,
					 TEEC_VALUE_OUTPUT,
					 TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)password;
	op.params[0].tmpref.size   = strlen(password);
	op.params[1].tmpref.buffer = objs;
	op.params[1].tmpref.size   = sizeof(objs);

	res = TEEC_InvokeCommand(&sess, DEMO_CMD_READ, &op, &err_origin);
	close_session(&ctx, &sess);

	if (res == TEEC_ERROR_ACCESS_DENIED) {
		fprintf(stderr, "✗ Wrong password.\n");
		return 1;
	}
	if (res == TEEC_ERROR_CORRUPT_OBJECT) {
		fprintf(stderr, "✗ Corrupt object detected! Storage may be damaged.\n");
		return 1;
	}
	if (res) {
		fprintf(stderr, "read failed: 0x%08x\n", res);
		return 1;
	}

	count = op.params[2].value.a;
	print_separator();
	printf("Read %u objects from TEE secure storage:\n", count);
	printf("  %3s  %-6s  %-10s  %-12s  %-16s  %s\n",
	       "idx", "ctr", "boot_cnt", "value", "magic", "cs");
	for (i = 0; i < count && i < DEMO_MAX_OBJECTS; i++)
		print_object(&objs[i], i);
	print_separator();
	return 0;
}

/* -------------------------------------------------------------------------
 * update
 * ---------------------------------------------------------------------- */

static int do_update(const char *password)
{
	TEEC_Context   ctx  = { };
	TEEC_Session   sess = { };
	TEEC_Operation op   = { };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       err_origin = 0;

	res = open_session(&ctx, &sess);
	if (res)
		return 1;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_NONE, TEEC_NONE, TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)password;
	op.params[0].tmpref.size   = strlen(password);

	res = TEEC_InvokeCommand(&sess, DEMO_CMD_UPDATE, &op, &err_origin);
	close_session(&ctx, &sess);

	if (res == TEEC_ERROR_ACCESS_DENIED) {
		fprintf(stderr, "✗ Wrong password.\n");
		return 1;
	}
	if (res) {
		fprintf(stderr, "update failed: 0x%08x\n", res);
		return 1;
	}
	printf("✓ All objects updated (power-safe write complete).\n");
	return 0;
}

/* -------------------------------------------------------------------------
 * demo — full lifecycle
 * ---------------------------------------------------------------------- */

static int do_demo(const char *password)
{
	int rc = 0;

	printf("\n══════════════════════════════════════════════════\n");
	printf(" W77Q QLIB TEE Demo — Full Lifecycle\n");
	printf("══════════════════════════════════════════════════\n\n");

	printf("[ Step 1: Initialize objects (first boot) ]\n");
	rc = do_init(password, 5, /*allow_existing=*/1);
	if (rc)
		return rc;

	printf("\n[ Step 2: List stored objects ]\n");
	rc = do_list(password);
	if (rc)
		return rc;

	printf("\n[ Step 3: Read objects (boot_count → 1) ]\n");
	rc = do_read(password);
	if (rc)
		return rc;

	printf("\n[ Step 4: Power-safe update (counter++, value+=index+1) ]\n");
	rc = do_update(password);
	if (rc)
		return rc;

	printf("\n[ Step 5: Read again — verify update persisted ]\n");
	rc = do_read(password);
	if (rc)
		return rc;

	printf("\n[ Step 6: Simulate next boot — second read (boot_count → 2) ]\n");
	rc = do_read(password);
	if (rc)
		return rc;

	printf("\n[ Step 7: Update again ]\n");
	rc = do_update(password);
	if (rc)
		return rc;

	printf("\n[ Step 8: Final read — verify cumulative state ]\n");
	rc = do_read(password);
	if (rc)
		return rc;

	printf("\n══════════════════════════════════════════════════\n");
	printf(" Demo complete.\n");
	printf(" Objects live in W77Q hardware-protected flash section.\n");
	printf(" State will persist across power cycles.\n");
	printf("══════════════════════════════════════════════════\n\n");
	return 0;
}

/* -------------------------------------------------------------------------
 * counter-read — no password, returns 64-bit value
 * ---------------------------------------------------------------------- */

static int do_counter_read(void)
{
	TEEC_Context   ctx  = { };
	TEEC_Session   sess = { };
	TEEC_Operation op   = { };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       err_origin = 0;
	uint64_t       value = 0;

	res = open_session(&ctx, &sess);
	if (res)
		return 1;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT,
					 TEEC_NONE, TEEC_NONE, TEEC_NONE);

	res = TEEC_InvokeCommand(&sess, DEMO_CMD_COUNTER_READ, &op,
				 &err_origin);
	close_session(&ctx, &sess);

	if (res) {
		fprintf(stderr, "counter-read failed: 0x%08x\n", res);
		return 1;
	}

	value = ((uint64_t)op.params[0].value.b << 32) |
		 (uint64_t)op.params[0].value.a;
	printf("✓ Monotonic counter: %" PRIu64 "\n", value);
	return 0;
}

/* -------------------------------------------------------------------------
 * counter-inc — password required, returns new value
 * ---------------------------------------------------------------------- */

static int do_counter_inc(const char *password)
{
	TEEC_Context   ctx  = { };
	TEEC_Session   sess = { };
	TEEC_Operation op   = { };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       err_origin = 0;
	uint64_t       new_value = 0;

	res = open_session(&ctx, &sess);
	if (res)
		return 1;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_VALUE_OUTPUT,
					 TEEC_NONE, TEEC_NONE);
	op.params[0].tmpref.buffer = (void *)password;
	op.params[0].tmpref.size   = strlen(password);

	res = TEEC_InvokeCommand(&sess, DEMO_CMD_COUNTER_INC, &op,
				 &err_origin);
	close_session(&ctx, &sess);

	if (res == TEEC_ERROR_ACCESS_DENIED) {
		fprintf(stderr, "✗ Wrong password.\n");
		return 1;
	}
	if (res) {
		fprintf(stderr, "counter-inc failed: 0x%08x\n", res);
		return 1;
	}

	new_value = ((uint64_t)op.params[1].value.b << 32) |
		     (uint64_t)op.params[1].value.a;
	printf("✓ Monotonic counter incremented → %" PRIu64 "\n", new_value);
	return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s set-password <password>\n"
		"      Store master password in TEE secure storage.\n"
		"\n"
		"  %s init <password> [count]\n"
		"      Create [count] (default 5, max %u) demo objects.\n"
		"\n"
		"  %s list <password>\n"
		"      List all stored object IDs.\n"
		"\n"
		"  %s read <password>\n"
		"      Read + verify all objects; increment boot_count.\n"
		"\n"
		"  %s update <password>\n"
		"      Power-safe update: counter++, value += (index+1).\n"
		"\n"
		"  %s counter-read\n"
		"      Read the TEE-only 64-bit monotonic counter (no password).\n"
		"\n"
		"  %s counter-inc <password>\n"
		"      Power-safe increment of the monotonic counter.\n"
		"\n"
		"  %s demo <password>\n"
		"      Full lifecycle demo (init → list → read → update → ...).\n"
		"\n"
		"First run on a fresh board:\n"
		"  %s set-password mysecret\n"
		"  %s demo mysecret\n",
		prog, prog, DEMO_MAX_OBJECTS,
		prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
	const char *cmd      = NULL;
	const char *password = NULL;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	cmd = argv[1];

	/* counter-read needs no password */
	if (strcmp(cmd, "counter-read") == 0)
		return do_counter_read();

	/* All other commands require a password argument */
	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	password = argv[2];

	if (strlen(password) == 0 || strlen(password) > DEMO_MAX_PASSWORD) {
		fprintf(stderr, "Password must be 1..%u characters.\n",
			DEMO_MAX_PASSWORD);
		return 1;
	}

	if (strcmp(cmd, "set-password") == 0)
		return do_set_password(password);

	if (strcmp(cmd, "init") == 0) {
		uint32_t count = 5;

		if (argc >= 4)
			count = (uint32_t)atoi(argv[3]);
		if (count < 1 || count > DEMO_MAX_OBJECTS) {
			fprintf(stderr,
				"count must be 1..%u\n", DEMO_MAX_OBJECTS);
			return 1;
		}
		return do_init(password, count, 0);
	}

	if (strcmp(cmd, "list") == 0)
		return do_list(password);

	if (strcmp(cmd, "read") == 0)
		return do_read(password);

	if (strcmp(cmd, "update") == 0)
		return do_update(password);

	if (strcmp(cmd, "counter-inc") == 0)
		return do_counter_inc(password);

	if (strcmp(cmd, "demo") == 0)
		return do_demo(password);

	fprintf(stderr, "Unknown command: %s\n\n", cmd);
	usage(argv[0]);
	return 1;
}
