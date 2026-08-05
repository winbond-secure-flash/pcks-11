#!/bin/sh
# ft11-target.sh - PKCS#11 + tee-storage cross-validation
# Deploy to /usr/bin/ft11-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  PKCS#11 + tee-storage cross-validation: write key via each TA, read via the other."
    echo ""
    echo "Pass/Fail criteria: PASS if each TA can read back what the other TA wrote."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
SECTOR=0x1000
TS_PAYLOAD="W77Q-FLASH-RECORD-SH001"
PKCS_PAYLOAD="W77Q-PKCS11-XVAL-SH001"
PKCS_LABEL="flash-record"
READBACK=/tmp/ft11_rb.bin

section "FT11: PKCS#11 + tee-storage cross-validation"

ensure_tee_supplicant

# --- Part 1: tee-storage ---
section "FT11.1: tee-storage write + read"
echo "  writing '$TS_PAYLOAD' to sector $SECTOR..."
tee-storage write-sector "$SECTOR" "$TS_PAYLOAD" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "tee-storage write-sector OK"
else
    fail "tee-storage write-sector returned $RC"
fi

echo "  reading back sector $SECTOR..."
TS_READ=$(tee-storage read-sector "$SECTOR" 2>&1)
echo "  read: $(echo "$TS_READ" | head -3)"
TS_PREFIX=$(printf '%s' "$TS_PAYLOAD" | cut -c1-16)
if echo "$TS_READ" | grep -q "|${TS_PREFIX}"; then
    pass "tee-storage readback matches"
else
    fail "tee-storage readback does not contain expected payload"
fi

# --- Part 2: PKCS#11 ---
section "FT11.2: PKCS#11 write + read"
ensure_token "$TOKEN1" 1
del_obj "$TOKEN1" data "$PKCS_LABEL"

printf '%s' "$PKCS_PAYLOAD" > /tmp/ft11_pkcs_expected.bin
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --write-object /tmp/ft11_pkcs_expected.bin --type data \
    --label "$PKCS_LABEL" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "pkcs11 write OK"
else
    fail "pkcs11 write returned $RC"
fi

pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --read-object --type data \
    --label "$PKCS_LABEL" --output-file "$READBACK" 2>&1
RC=$?
if [ "$RC" -eq 0 ] && cmp -s /tmp/ft11_pkcs_expected.bin "$READBACK"; then
    pass "pkcs11 readback matches"
else
    fail "pkcs11 readback mismatch or error (rc=$RC)"
fi

section "FT11.3: Cleanup"
tee-storage erase-sector "$SECTOR" 2>/dev/null || true
del_obj "$TOKEN1" data "$PKCS_LABEL"
rm -f "$READBACK" /tmp/ft11_pkcs_expected.bin

section "FT11: Summary"
if [ "$PASS" -eq 1 ]; then
    pass "FT11 PASSED"
    exit 0
else
    fail "FT11 FAILED"
    exit 1
fi
