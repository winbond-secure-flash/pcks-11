// SPDX-License-Identifier: BSD-2-Clause
/*
 * w77q_fs_test.c — Linux host application for the w77q_fs filesystem tests.
 *
 * Usage:
 *   w77q-fs-test            Run all test cases, report PASS/FAIL per TC.
 *   w77q-fs-test basic      Run TC_BASIC only.
 *   w77q-fs-test update     Run TC_UPDATE only.
 *   w77q-fs-test rename     Run TC_RENAME only.
 *   w77q-fs-test enum       Run TC_ENUM only.
 *   w77q-fs-test large      Run TC_LARGE only.
 *   w77q-fs-test overwrite  Run TC_OVERWRITE only.
 *   w77q-fs-test truncate   Run TC_TRUNCATE only.
 *
 * Exit code: number of failed test cases (0 = all passed).
 *
 * Tests exercise the w77q_fs secure-storage backend (TEE_STORAGE_PRIVATE)
 * running inside OP-TEE on the W77Q SPI NOR flash section.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <tee_client_api.h>
#include "../../ta/include/w77q_fs_test_ta.h"

static const TEEC_UUID g_ta_uuid = W77Q_FS_TEST_TA_UUID;

/* ANSI colour helpers — disabled when stdout is not a tty */
#define COL_GREEN  "\033[32m"
#define COL_RED    "\033[31m"
#define COL_RESET  "\033[0m"
#define COL_BOLD   "\033[1m"

static int g_color = 0;

static const char *green(void) { return g_color ? COL_GREEN : ""; }
static const char *red(void)   { return g_color ? COL_RED   : ""; }
static const char *bold(void)  { return g_color ? COL_BOLD  : ""; }
static const char *rst(void)   { return g_color ? COL_RESET : ""; }

/* ---- Names --------------------------------------------------------------- */

static const char *tc_names[W77Q_FS_TC_COUNT] = {
	"basic",
	"update",
	"rename",
	"enum",
	"large",
	"overwrite",
	"truncate",
};

static int name_to_cmd(const char *name)
{
	uint32_t i = 0;

	for (i = 0; i < W77Q_FS_TC_COUNT; i++)
		if (strcmp(name, tc_names[i]) == 0)
			return (int)i;
	return -1;
}

/* ---- TEE helpers --------------------------------------------------------- */

static int open_session(TEEC_Context *ctx, TEEC_Session *sess)
{
	TEEC_Result res  = TEEC_SUCCESS;
	uint32_t    orig = 0;

	res = TEEC_InitializeContext(NULL, ctx);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_InitializeContext: 0x%08x\n", res);
		return -1;
	}
	res = TEEC_OpenSession(ctx, sess, &g_ta_uuid,
			       TEEC_LOGIN_PUBLIC, NULL, NULL, &orig);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_OpenSession: 0x%08x (orig 0x%08x)\n",
			res, orig);
		TEEC_FinalizeContext(ctx);
		return -1;
	}
	return 0;
}

static void close_session(TEEC_Context *ctx, TEEC_Session *sess)
{
	TEEC_CloseSession(sess);
	TEEC_FinalizeContext(ctx);
}

/* Run a single TC by command number.  Returns 0 on PASS, 1 on FAIL. */
static int run_tc(TEEC_Session *sess, uint32_t cmd, const char *name)
{
	TEEC_Operation op   = { 0 };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       orig = 0;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE,
					 TEEC_NONE, TEEC_NONE);

	res = TEEC_InvokeCommand(sess, cmd, &op, &orig);

	if (res == TEEC_SUCCESS) {
		printf("  w77q-fs: %-12s %sPASS%s\n",
		       name, green(), rst());
		return 0;
	}

	printf("  w77q-fs: %-12s %sFAIL%s  (0x%08x)\n",
	       name, red(), rst(), res);
	return 1;
}

/* Run TC_ALL via a single TA invocation.  Returns number of failures. */
static int run_all(TEEC_Session *sess)
{
	TEEC_Operation op   = { 0 };
	TEEC_Result    res  = TEEC_SUCCESS;
	uint32_t       orig = 0;
	uint32_t       mask = 0;
	uint32_t       i    = 0;
	int            fail = 0;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT, TEEC_NONE,
					 TEEC_NONE, TEEC_NONE);

	res = TEEC_InvokeCommand(sess, W77Q_FS_TC_ALL, &op, &orig);
	mask = op.params[0].value.a;

	/* Print individual results from the bitmask */
	for (i = 0; i < W77Q_FS_TC_COUNT; i++) {
		int failed = (mask >> i) & 1U;

		printf("  w77q-fs: %-12s %s%s%s\n",
		       tc_names[i],
		       failed ? red() : green(),
		       failed ? "FAIL" : "PASS",
		       rst());
		if (failed)
			fail++;
	}

	/* If the TA itself returned an error but mask is empty, count it */
	if (res != TEEC_SUCCESS && fail == 0) {
		fprintf(stderr, "TC_ALL TA error: 0x%08x\n", res);
		fail = 1;
	}

	return fail;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	TEEC_Context ctx  = { 0 };
	TEEC_Session sess = { 0 };
	int          fail = 0;

	/* Enable colour if stdout is a terminal */
	g_color = isatty(fileno(stdout));

	if (open_session(&ctx, &sess) != 0)
		return 1;

	printf("\n%sw77q-fs storage backend test%s\n", bold(), rst());
	printf("─────────────────────────────\n");

	if (argc < 2) {
		/* No argument: run all */
		fail = run_all(&sess);
	} else {
		/* Named test case */
		int cmd = name_to_cmd(argv[1]);

		if (cmd < 0) {
			fprintf(stderr,
				"Unknown test '%s'.\n"
				"Available: basic update rename enum large "
				"overwrite truncate\n",
				argv[1]);
			close_session(&ctx, &sess);
			return 1;
		}
		fail = run_tc(&sess, (uint32_t)cmd, argv[1]);
	}

	printf("─────────────────────────────\n");
	if (fail == 0)
		printf("%sw77q-fs: all tests PASSED%s\n\n", green(), rst());
	else
		printf("%sw77q-fs: %d test(s) FAILED%s\n\n", red(), fail, rst());

	close_session(&ctx, &sess);
	return fail;
}
