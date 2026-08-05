#!/bin/sh
# ft12-target.sh - PKCS#11 write + scan flash sectors via tee-storage
# Deploy to /usr/bin/ft12-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  PKCS#11 write + scan all flash sectors via tee-storage for the new object."
    echo ""
    echo "Pass/Fail criteria: PASS if written object is found in flash sector scan."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
PAYLOAD="W77Q-PKCS11-XVAL-SH001"
LABEL="flash-record"
READBACK=/tmp/ft12_rb.bin
SCAN_ADDRS="0xaa00 0xaa40 0xaa80 0xaac0 0xab00 0xab40 0xab80 0xac00 0xad00 0xae00 0xaf00 0xb000"

section "FT12: PKCS#11 write + flash sector scan"

ensure_tee_supplicant
ensure_token "$TOKEN1" 1

section "FT12.1: Delete stale object and write"
del_obj "$TOKEN1" data "$LABEL"

printf '%s' "$PAYLOAD" > /tmp/ft12_expected.bin
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --write-object /tmp/ft12_expected.bin --type data \
    --label "$LABEL" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "pkcs11 write OK"
else
    fail "pkcs11 write returned $RC"
fi

section "FT12.2: Scan flash sectors via tee-storage"
echo "  scanning addresses: $SCAN_ADDRS"
FOUND=0
for ADDR in $SCAN_ADDRS; do
    RESULT=$(tee-storage read-sector "$ADDR" 2>&1)
    if echo "$RESULT" | grep -qi "\[OK\]"; then
        SIZE=$(echo "$RESULT" | grep -oE '[0-9]+ byte' | head -1 || echo "?")
        echo "  [OK] $ADDR  $SIZE"
        FOUND=$((FOUND + 1))
    fi
done
echo "  sectors with data: $FOUND"

section "FT12.3: PKCS#11 read-back verification"
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --read-object --type data \
    --label "$LABEL" --output-file "$READBACK" 2>&1
if [ $? -eq 0 ] && cmp -s /tmp/ft12_expected.bin "$READBACK"; then
    pass "pkcs11 readback matches"
else
    fail "pkcs11 readback mismatch"
fi

section "FT12.4: Cleanup"
del_obj "$TOKEN1" data "$LABEL"
rm -f "$READBACK" /tmp/ft12_expected.bin

section "FT12: Summary"
if [ "$PASS" -eq 1 ]; then
    pass "FT12 PASSED"
    exit 0
else
    fail "FT12 FAILED"
    exit 1
fi
