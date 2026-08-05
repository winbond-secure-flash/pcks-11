/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * tee_demo_ta.h — Shared definitions for the W77Q TEE demo TA.
 *
 * Include in both the Trusted Application (Secure World) and the
 * Linux host application (Normal World via libteec).
 *
 * Demo scenario
 * -------------
 * 1. set-password  — store master password in TEE secure storage
 * 2. init          — create N demo objects in TEE secure storage
 * 3. list          — enumerate stored object IDs
 * 4. read          — read + verify all objects (boot_count incremented)
 * 5. update        — power-safe update: each object gets counter++ and
 *                    value += index+1  (uses tmp-object + rename for atomicity)
 * 6. counter-read  — read TEE-only 64-bit monotonic counter (no password)
 * 7. counter-inc   — password-gated power-safe increment of the counter
 * 8. demo          — full lifecycle: init → list → read → update → read
 *
 * Security layers
 * ---------------
 *  TA secure storage (TEE_STORAGE_PRIVATE)
 *    → OP-TEE AES-GCM encryption (software, Secure World)
 *      → W77Q QLIB session (hardware MAC + anti-rollback, Secure World)
 *        → W77Q hardware OTP section key (burned at factory)
 */

#ifndef TEE_DEMO_TA_H
#define TEE_DEMO_TA_H

/*
 * Demo TA UUID: b1c2d3e4-f5a6-7890-abcd-ef0123456789
 *
 * As TEEC_UUID struct  { 0xb1c2d3e4, 0xf5a6, 0x7890,
 *                        { 0xab,0xcd,0xef,0x01,0x23,0x45,0x67,0x89 } }
 * TA binary filename:  b1c2d3e4-f5a6-7890-abcd-ef0123456789.ta
 */
#define DEMO_TA_UUID \
	{ 0xb1c2d3e4U, 0xf5a6U, 0x7890U, \
	  { 0xabU, 0xcdU, 0xefU, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U } }

/*---------------------------------------------------------------------------
 * Commands
 *--------------------------------------------------------------------------*/

/*
 * DEMO_CMD_SET_PASSWORD — Store the master password in secure storage.
 *   params[0]: MEMREF_INPUT  password (max DEMO_MAX_PASSWORD bytes)
 *
 * First call creates the password object.  Subsequent calls replace it.
 * Returns TEE_SUCCESS.
 */
#define DEMO_CMD_SET_PASSWORD   0U

/*
 * DEMO_CMD_INIT — Create N demo objects (fails if objects already exist).
 *   params[0]: MEMREF_INPUT  password
 *   params[1]: VALUE_INPUT   a = number of objects to create (1..DEMO_MAX_OBJECTS)
 *
 * Returns TEE_SUCCESS or TEE_ERROR_ACCESS_DENIED (wrong password).
 */
#define DEMO_CMD_INIT           1U

/*
 * DEMO_CMD_LIST — Enumerate stored object IDs.
 *   params[0]: MEMREF_INPUT   password
 *   params[1]: MEMREF_OUTPUT  buffer: array of DEMO_OBJECT_ID_LEN-byte IDs
 *   params[2]: VALUE_OUTPUT   a = number of objects found
 *
 * Returns TEE_SUCCESS or TEE_ERROR_ACCESS_DENIED.
 */
#define DEMO_CMD_LIST           2U

/*
 * DEMO_CMD_READ — Read and verify all demo objects.
 *   params[0]: MEMREF_INPUT   password
 *   params[1]: MEMREF_OUTPUT  buffer: array of struct demo_object
 *   params[2]: VALUE_OUTPUT   a = number of objects returned
 *
 * Also increments boot_count in each object.
 * Returns TEE_SUCCESS or TEE_ERROR_ACCESS_DENIED.
 */
#define DEMO_CMD_READ           3U

/*
 * DEMO_CMD_UPDATE — Power-safe update: increment counter, add value.
 *   params[0]: MEMREF_INPUT  password
 *
 * Each object is updated atomically:
 *   1. Write updated data to "w77q_demo_NN.tmp"
 *   2. Delete "w77q_demo_NN"
 *   3. Rename "w77q_demo_NN.tmp" → "w77q_demo_NN"
 *
 * Returns TEE_SUCCESS or TEE_ERROR_ACCESS_DENIED.
 */
#define DEMO_CMD_UPDATE         4U

/*
 * DEMO_CMD_COUNTER_READ — Read the TEE-only 64-bit monotonic counter.
 *   params[0]: VALUE_OUTPUT  a = counter low 32 bits
 *                            b = counter high 32 bits
 *
 * No password required.  Counter object "w77q_mc" lives in
 * TEE_STORAGE_PRIVATE (QLIB-backed W77Q flash, Secure World only).
 * Normal World has no direct flash access (LIFEC-locked by TF-A BL31).
 * Returns value 0 if counter has never been initialised.
 * Returns TEE_SUCCESS.
 */
#define DEMO_CMD_COUNTER_READ   5U

/*
 * DEMO_CMD_COUNTER_INC — Increment the TEE-only monotonic counter.
 *   params[0]: MEMREF_INPUT  password
 *   params[1]: VALUE_OUTPUT  a = new value low 32 bits
 *                            b = new value high 32 bits
 *
 * Uses an append-only log (MC_LOG_SLOTS × 16-byte records in one
 * pre-allocated persistent object sized to fill the full 4-sector slot).
 * Each increment appends one record to the next empty slot — no sector
 * erase and no TOC flush per increment (object size never changes).
 * Every MC_LOG_SLOTS (1019) increments a compaction is done
 * (4 data-sector erases + 1 TOC flush).
 *
 * Anti-rollback: the append-only log itself provides the monotonic guarantee.
 * Each slot can only be written once (NOR: 1→0, never 0→1 without erase),
 * and an erase requires the QLIB session key burned at provisioning time.
 * An attacker cannot replay an older counter value without erasing the log.
 *
 * Returns TEE_SUCCESS or TEE_ERROR_ACCESS_DENIED (wrong password).
 */
#define DEMO_CMD_COUNTER_INC    6U

/*---------------------------------------------------------------------------
 * Constants
 *--------------------------------------------------------------------------*/

#define DEMO_MAX_OBJECTS        8U
#define DEMO_MAX_PASSWORD       64U
#define DEMO_OBJECT_ID_LEN      16U   /* "w77q_demo_NN\0" padded to 16 */
#define DEMO_DATA_MAGIC         0xD3E40789U
#define DEMO_MC_MAGIC           0xC04A7E01U  /* monotonic counter object magic */

/*---------------------------------------------------------------------------
 * Persistent object data layout (stored in TEE secure storage)
 *--------------------------------------------------------------------------*/

struct demo_object {
	uint32_t magic;       /**< DEMO_DATA_MAGIC — integrity marker        */
	uint32_t index;       /**< Object index 0..N-1                       */
	uint32_t counter;     /**< Incremented by DEMO_CMD_UPDATE             */
	uint32_t boot_count;  /**< Incremented by DEMO_CMD_READ               */
	uint64_t value;       /**< Payload: (index+1)*1000 + counter          */
	uint8_t  checksum;    /**< XOR of bytes [4..23] — all fields except magic and checksum+pad */
	uint8_t  _pad[7];
} __attribute__((packed));

/* Monotonic counter object stored as "w77q_mc" in TEE_STORAGE_PRIVATE */
struct demo_mc {
	uint32_t magic;   /**< DEMO_MC_MAGIC — integrity marker (old format) */
	uint64_t value;   /**< 64-bit monotonic counter value                */
	uint8_t  checksum;/**< XOR of bytes [4..11] (value only)             */
	uint8_t  _pad[3];
} __attribute__((packed));

/*
 * Append-only log counter record — new format stored in "w77q_mc".
 * Object is pre-allocated at MC_LOG_OBJ_SZ bytes (all 0xFF) to fill the
 * full 4-sector (16 KB) slot.  Max data per slot = 4×4096 - 68 (HDR) =
 * 16316 bytes → 16316 / 16 = 1019 records before compaction is needed.
 * Each increment writes one record at the next empty slot.
 * Object size never changes → no TOC erase per increment.
 */
#define MC_LOG_MAGIC    0xC04A7E02U   /* distinct from DEMO_MC_MAGIC */
#define MC_LOG_SLOTS    1019U         /* floor((4*4096 - 68) / 16) = 1019 */
#define MC_LOG_REC_SIZE 16U
#define MC_LOG_OBJ_SZ   (MC_LOG_SLOTS * MC_LOG_REC_SIZE)  /* 16304 bytes */

struct mc_log_record {
	uint32_t magic;   /**< MC_LOG_MAGIC = valid; 0xFFFFFFFF = empty */
	uint64_t value;   /**< counter value                             */
	uint32_t check;   /**< XOR of preceding 12 bytes                */
} __attribute__((packed));

#endif /* TEE_DEMO_TA_H */
