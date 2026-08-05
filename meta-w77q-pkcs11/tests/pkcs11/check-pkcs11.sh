#!/bin/sh
# check-pkcs11.sh — On-target PKCS#11 smoke test for Sparrow Hawk
#
# Tests the OP-TEE PKCS#11 TA (fd02c9da-…) via libckteec.so using pkcs11-tool.
# Performs: install check → token init → RSA-2048 keygen → sign → verify →
#           EC P-256 keygen → AES-128 keygen → encrypt → decrypt → cleanup.
#
# Usage: check-pkcs11.sh [SO_PIN] [USER_PIN]
#   Default SO_PIN : 12345678
#   Default USER_PIN: 87654321

set -e

MODULE=/usr/lib/libckteec.so
TA_PATH=/usr/lib/optee_armtz/fd02c9da-306c-48c7-a49c-bbd827ae86ee.ta
TOKEN_LABEL="optee-test-token"
SO_PIN="${1:-12345678}"
USER_PIN="${2:-87654321}"
TMPDIR=/tmp/pkcs11-check
PASS=0
FAIL=0

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m'

pass() { echo "${GREEN}[PASS]${NC} $1"; PASS=$((PASS+1)); }
fail() { echo "${RED}[FAIL]${NC} $1"; FAIL=$((FAIL+1)); }
info() { echo "${YELLOW}[INFO]${NC} $1"; }

# Print command, run it, show all output; stdout passthrough for grep/pipe callers
p11() {
    printf "${CYAN}▶ pkcs11-tool --module %s %s${NC}\n" "$MODULE" "$*" >&2
    pkcs11-tool --module "$MODULE" "$@" 2>&1 | tee /dev/tty
}

echo "========================================"
echo " OP-TEE PKCS#11 Smoke Test"
echo "========================================"

# ── 1. Prerequisites ────────────────────────────────────────────────────────

info "Checking prerequisites..."

if [ -c /dev/tee0 ]; then
    pass "/dev/tee0 exists"
else
    fail "/dev/tee0 missing — TEE driver not loaded"; exit 1
fi

if systemctl is-active --quiet optee.service 2>/dev/null; then
    pass "tee-supplicant is running (optee.service)"
elif pgrep tee-supplicant >/dev/null 2>&1; then
    pass "tee-supplicant process found"
else
    fail "tee-supplicant is NOT running"; exit 1
fi

if [ -f "$TA_PATH" ]; then
    pass "PKCS#11 TA present: $TA_PATH"
else
    fail "PKCS#11 TA missing: $TA_PATH"; exit 1
fi

if [ -f "$MODULE" ]; then
    pass "libckteec present: $MODULE"
else
    fail "libckteec missing: $MODULE"; exit 1
fi

if command -v pkcs11-tool >/dev/null 2>&1; then
    pass "pkcs11-tool available ($(pkcs11-tool --version 2>&1 | head -1))"
else
    fail "pkcs11-tool not found — install opensc"; exit 1
fi

mkdir -p "$TMPDIR"

# ── 2. Slot / Module enumeration ────────────────────────────────────────────

info "Enumerating slots..."
if p11 --list-slots | grep -q "Slot"; then
    pass "Slot enumeration succeeded"
    p11 --list-slots | grep -E "Slot|token|Flags" | sed 's/^/        /'
else
    fail "No slots found — PKCS#11 TA may not be responding"; exit 1
fi

# ── 3. Token initialization ──────────────────────────────────────────────────

info "Initializing token on slot 0 (label: $TOKEN_LABEL)..."
if p11 --init-token --slot-index 0 --label "$TOKEN_LABEL" \
        --so-pin "$SO_PIN" | grep -q "oken initialized"; then
    pass "Token initialized"
else
    # Token may already be initialized from a previous run — try to continue
    info "Token may already exist; continuing"
fi

info "Setting user PIN..."
if p11 --init-pin --token-label "$TOKEN_LABEL" \
        --login --login-type so --so-pin "$SO_PIN" --new-pin "$USER_PIN" \
        | grep -q "User PIN successfully initialized"; then
    pass "User PIN set"
else
    info "User PIN may already be set; continuing"
fi

# ── 4. RSA-2048 key pair + sign/verify ──────────────────────────────────────

info "Generating RSA-2048 key pair..."
if p11 --login --token-label "$TOKEN_LABEL" --pin "$USER_PIN" \
        --keypairgen --key-type rsa:2048 --label "test-rsa" --id 01 --usage-sign \
        | grep -q "Key pair generated"; then
    pass "RSA-2048 key pair generated"
else
    fail "RSA-2048 key generation failed"
fi

echo "RSA2048 test vector" > "$TMPDIR/data.txt"

info "Signing with RSA-2048 / SHA256-RSA-PKCS..."
if p11 --login --token-label "$TOKEN_LABEL" --pin "$USER_PIN" \
        --sign --mechanism SHA256-RSA-PKCS --label "test-rsa" \
        --input-file "$TMPDIR/data.txt" \
        --output-file "$TMPDIR/rsa-sig.bin" | grep -qE "Using|signature"; then
    pass "RSA sign succeeded ($(wc -c < "$TMPDIR/rsa-sig.bin") bytes)"
else
    fail "RSA sign failed"
fi

info "Verifying RSA-2048 signature..."
if p11 --token-label "$TOKEN_LABEL" \
        --verify --mechanism SHA256-RSA-PKCS --label "test-rsa" \
        --input-file "$TMPDIR/data.txt" \
        --signature-file "$TMPDIR/rsa-sig.bin" | grep -q "Signature is valid"; then
    pass "RSA signature verified"
else
    fail "RSA signature verification failed"
fi

# ── 5. EC P-256 key pair + sign/verify ──────────────────────────────────────

info "Generating EC P-256 (prime256v1) key pair..."
if p11 --login --token-label "$TOKEN_LABEL" --pin "$USER_PIN" \
        --keypairgen --key-type EC:prime256v1 --label "test-ec" --id 02 --usage-sign \
        | grep -q "Key pair generated"; then
    pass "EC P-256 key pair generated"
else
    fail "EC P-256 key generation failed"
fi

echo "ECDSA test vector" > "$TMPDIR/ec-data.txt"

info "Signing with EC / ECDSA..."
if p11 --login --token-label "$TOKEN_LABEL" --pin "$USER_PIN" \
        --sign --mechanism ECDSA --label "test-ec" \
        --input-file "$TMPDIR/ec-data.txt" \
        --output-file "$TMPDIR/ec-sig.bin" | grep -qE "Using|signature"; then
    pass "ECDSA sign succeeded ($(wc -c < "$TMPDIR/ec-sig.bin") bytes)"
else
    fail "ECDSA sign failed"
fi

info "Verifying ECDSA signature..."
if p11 --token-label "$TOKEN_LABEL" \
        --verify --mechanism ECDSA --label "test-ec" \
        --input-file "$TMPDIR/ec-data.txt" \
        --signature-file "$TMPDIR/ec-sig.bin" | grep -q "Signature is valid"; then
    pass "ECDSA signature verified"
else
    fail "ECDSA signature verification failed"
fi

# ── 6. AES-128 secret key + encrypt/decrypt ──────────────────────────────────

info "Generating AES-128 secret key..."
if p11 --login --token-label "$TOKEN_LABEL" --pin "$USER_PIN" \
        --keygen --key-type AES:16 --label "test-aes" --id 03 \
        --allowed-mechanisms AES-CBC \
        | grep -q "Key generated"; then
    pass "AES-128 key generated"
else
    fail "AES-128 key generation failed"
fi

# AES-CBC requires block-aligned plaintext (16 bytes) and an IV
AES_IV="00000000000000000000000000000000"
printf "Hello PKCS11 AES!!!" > "$TMPDIR/plain.txt"   # 19 bytes — pad to 32
# Pad to 32 bytes with nulls so AES-CBC block alignment is satisfied
dd if=/dev/zero bs=1 count=13 >> "$TMPDIR/plain.txt" 2>/dev/null

info "Encrypting with AES-128-CBC..."
if p11 --login --token-label "$TOKEN_LABEL" --pin "$USER_PIN" \
        --encrypt --mechanism AES-CBC --iv "$AES_IV" --label "test-aes" \
        --input-file "$TMPDIR/plain.txt" \
        --output-file "$TMPDIR/cipher.bin" | grep -qE "Using|encrypted|[0-9]"; then
    pass "AES encrypt succeeded ($(wc -c < "$TMPDIR/cipher.bin") bytes ciphertext)"
else
    fail "AES encrypt failed"
fi

info "Decrypting with AES-128-CBC..."
if p11 --login --token-label "$TOKEN_LABEL" --pin "$USER_PIN" \
        --decrypt --mechanism AES-CBC --iv "$AES_IV" --label "test-aes" \
        --input-file "$TMPDIR/cipher.bin" \
        --output-file "$TMPDIR/decrypted.bin" | grep -qE "Using|decrypted|[0-9]"; then
    if cmp -s "$TMPDIR/plain.txt" "$TMPDIR/decrypted.bin"; then
        pass "AES decrypt succeeded — plaintext matches"
    else
        fail "AES decrypt: plaintext mismatch"
    fi
else
    fail "AES decrypt failed"
fi

# ── 7. List all objects ───────────────────────────────────────────────────────

info "Listing all PKCS#11 objects in token..."
p11 --login --token-label "$TOKEN_LABEL" --pin "$USER_PIN" \
    --list-objects | sed 's/^/        /'

# ── 8. Cleanup ────────────────────────────────────────────────────────────────

rm -rf "$TMPDIR"

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "========================================"
echo " Results: ${PASS} passed, ${FAIL} failed"
echo "========================================"

[ "$FAIL" -eq 0 ]
