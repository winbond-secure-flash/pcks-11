#!/bin/sh
# verify-install.sh — Stage-by-stage installation verification for W77Q PKCS#11
#
# Runs on the target board. Checks each layer of the stack in order and reports
# exactly which stage is broken. Exits 0 only if all stages pass.
#
# Usage:
#   verify-install.sh [--verbose]
#
# Deploy to /usr/bin/ via the pkcs11-tests recipe.
#
# Copyright (c) 2026 Winbond Electronics Corporation. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause

set -u

VERBOSE=0
[ "${1:-}" = "--verbose" ] && VERBOSE=1

# ── Colours ────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

STAGE=0
PASS_COUNT=0
FAIL_COUNT=0
FIRST_FAILURE=""

# ── Helpers ────────────────────────────────────────────────────────────────
stage_pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    printf "${GREEN}[PASS]${NC} Stage %d: %s\n" "$STAGE" "$1"
}

stage_fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    printf "${RED}[FAIL]${NC} Stage %d: %s\n" "$STAGE" "$1"
    if [ -z "$FIRST_FAILURE" ]; then
        FIRST_FAILURE="Stage $STAGE: $1"
    fi
    printf "${YELLOW}       Hint: %s${NC}\n" "$2"
}

stage_skip() {
    printf "${YELLOW}[SKIP]${NC} Stage %d: %s (previous stage failed)\n" "$STAGE" "$1"
}

verbose() {
    [ "$VERBOSE" -eq 1 ] && printf "${CYAN}       %s${NC}\n" "$1"
}

# ── Stage 1: OP-TEE device node ────────────────────────────────────────────
STAGE=1
if [ -c /dev/tee0 ]; then
    stage_pass "OP-TEE device present (/dev/tee0)"
    verbose "$(ls -l /dev/tee0)"
else
    stage_fail "OP-TEE device missing (/dev/tee0)" \
        "BL32 (OP-TEE) may not have loaded. Check BL31→BL32 handoff and DTB tee node."
fi

# ── Stage 2: tee-supplicant running ────────────────────────────────────────
STAGE=2
if [ "$FAIL_COUNT" -gt 0 ] && [ "$STAGE" -eq 2 ]; then
    stage_skip "tee-supplicant"
else
    if systemctl is-active tee-supplicant >/dev/null 2>&1 || \
       pidof tee-supplicant >/dev/null 2>&1; then
        stage_pass "tee-supplicant running"
    else
        stage_fail "tee-supplicant not running" \
            "Run: systemctl start tee-supplicant (or check /etc/init.d/tee-supplicant)"
    fi
fi

# ── Stage 3: PKCS#11 shared library present ────────────────────────────────
STAGE=3
MOD=/usr/lib/libckteec.so
[ -e "$MOD" ] || MOD=/usr/lib/libckteec.so.0
if [ -e "$MOD" ]; then
    stage_pass "PKCS#11 client library present ($MOD)"
    verbose "$(ls -l $MOD)"
else
    stage_fail "PKCS#11 client library missing" \
        "libckteec.so not found. Check that optee-client recipe built correctly."
fi

# ── Stage 4: PKCS#11 TA deployed ──────────────────────────────────────────
STAGE=4
TA_UUID="fd02c9da-306c-48c7-a49c-bbd827ae86ee"
TA_PATH="/usr/lib/optee_armtz/${TA_UUID}.ta"
if [ -e "$TA_PATH" ]; then
    stage_pass "PKCS#11 TA deployed ($TA_UUID)"
    verbose "$(ls -l $TA_PATH)"
else
    stage_fail "PKCS#11 TA not found at $TA_PATH" \
        "Ensure optee-os recipe built the PKCS#11 TA and it was installed to rootfs."
fi

# ── Stage 5: PKCS#11 slot enumeration ─────────────────────────────────────
STAGE=5
if [ "$FAIL_COUNT" -gt 0 ]; then
    stage_skip "PKCS#11 slot enumeration"
else
    SLOT_OUTPUT=$(pkcs11-tool --module "$MOD" -L 2>&1) || true
    if echo "$SLOT_OUTPUT" | grep -qi "slot"; then
        stage_pass "PKCS#11 slot enumeration succeeded"
        verbose "$(echo "$SLOT_OUTPUT" | head -5)"
    else
        stage_fail "PKCS#11 slot enumeration failed" \
            "TA may not load. Check: tee-supplicant logs, dmesg for OP-TEE errors."
        verbose "$SLOT_OUTPUT"
    fi
fi

# ── Stage 6: W77Q flash reachable (QLIB connect) ──────────────────────────
STAGE=6
if [ "$FAIL_COUNT" -gt 0 ]; then
    stage_skip "W77Q flash (QLIB connect)"
else
    # Token init exercises QLIB connect internally; we test via a lightweight
    # pkcs11-tool operation that triggers TA → w77q_qlib → SPI access.
    TOKEN_LABEL="verify-install-token"
    SOPIN="12345678"
    INIT_OUT=$(pkcs11-tool --module "$MOD" --slot-index 0 \
        --init-token --label "$TOKEN_LABEL" --so-pin "$SOPIN" 2>&1) || true
    if echo "$INIT_OUT" | grep -qi "token initialized\|already initialized\|CKR_OK"; then
        stage_pass "W77Q flash reachable (token init succeeded via QLIB)"
        verbose "$INIT_OUT"
    elif echo "$INIT_OUT" | grep -qi "device error\|CKR_DEVICE_ERROR"; then
        stage_fail "W77Q flash unreachable (CKR_DEVICE_ERROR)" \
            "QLIB cannot connect to W77Q. Check: SPI wiring, LIFEC config, key provisioning."
        verbose "$INIT_OUT"
    else
        # Could be already initialized — try listing objects instead
        LIST_OUT=$(pkcs11-tool --module "$MOD" -T 2>&1) || true
        if echo "$LIST_OUT" | grep -qi "token label"; then
            stage_pass "W77Q flash reachable (existing token found)"
            verbose "$LIST_OUT"
        else
            stage_fail "W77Q flash status unclear" \
                "Unexpected response. Run with --verbose and check output."
            verbose "$INIT_OUT"
        fi
    fi
fi

# ── Stage 7: Key generation (full path) ───────────────────────────────────
STAGE=7
if [ "$FAIL_COUNT" -gt 0 ]; then
    stage_skip "Key generation (full data path)"
else
    UPIN="87654321"
    # Ensure user PIN is set
    pkcs11-tool --module "$MOD" --token-label "$TOKEN_LABEL" \
        --so-pin "$SOPIN" --init-pin --new-pin "$UPIN" >/dev/null 2>&1 || true

    KEYGEN_OUT=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN_LABEL" \
        --login --pin "$UPIN" \
        --keypairgen --key-type EC:prime256v1 \
        --label "verify-install-key" --id 99 2>&1) || true
    if echo "$KEYGEN_OUT" | grep -qi "key pair generated\|CKR_OK"; then
        stage_pass "EC P-256 key generation succeeded (full data path verified)"
        verbose "$KEYGEN_OUT"
    else
        stage_fail "Key generation failed" \
            "PKCS#11 TA + QLIB + W77Q storage path broken. Check dmesg for OP-TEE panics."
        verbose "$KEYGEN_OUT"
    fi

    # Cleanup: delete the test key
    pkcs11-tool --module "$MOD" --token-label "$TOKEN_LABEL" \
        --login --pin "$UPIN" \
        --delete-object --type privkey --label "verify-install-key" >/dev/null 2>&1 || true
    pkcs11-tool --module "$MOD" --token-label "$TOKEN_LABEL" \
        --login --pin "$UPIN" \
        --delete-object --type pubkey --label "verify-install-key" >/dev/null 2>&1 || true
fi

# ── Summary ────────────────────────────────────────────────────────────────
echo ""
echo "========================================"
printf " Results: ${GREEN}%d PASS${NC}, ${RED}%d FAIL${NC} (of %d stages)\n" \
    "$PASS_COUNT" "$FAIL_COUNT" "$STAGE"
echo "========================================"

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo ""
    printf "${RED}First failure: %s${NC}\n" "$FIRST_FAILURE"
    echo ""
    echo "Troubleshooting order:"
    echo "  1. Fix the first failing stage before investigating later stages."
    echo "  2. Run with --verbose for detailed output."
    echo "  3. Check dmesg and journalctl -u tee-supplicant for errors."
    exit 1
else
    echo ""
    printf "${GREEN}All stages passed. W77Q PKCS#11 stack is operational.${NC}\n"
    exit 0
fi
