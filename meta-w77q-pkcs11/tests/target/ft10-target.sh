#!/bin/sh
# ft10-target.sh - PKCS#11 write + read round-trip
# Deploy to /usr/bin/ft10-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  PKCS#11 write + read round-trip: write a data object, read it back, verify match."
    echo ""
    echo "Pass/Fail criteria: PASS if written and read-back values match exactly."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
PAYLOAD="W77Q-PKCS11-XVAL-SH001"
LABEL="flash-record"
READBACK=/tmp/ft10_rb.bin
EXPECTED=/tmp/ft10_expected.bin

section "FT10: PKCS#11 write + read round-trip"

ensure_tee_supplicant
ensure_token "$TOKEN0" 0

section "FT10.1: Delete stale object"
del_obj "$TOKEN0" data "$LABEL"

section "FT10.2: Write PKCS#11 data object"
printf '%s' "$PAYLOAD" > "$EXPECTED"
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN0" --login --pin "$UPIN" \
    --write-object "$EXPECTED" --type data \
    --label "$LABEL" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "write succeeded"
else
    fail "write returned $RC"
fi

section "FT10.3: Read back PKCS#11 data object"
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN0" --login --pin "$UPIN" \
    --read-object --type data \
    --label "$LABEL" --output-file "$READBACK" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "read-back succeeded"
else
    fail "read-back returned $RC"
    exit 1
fi

section "FT10.4: Compare original vs readback"
echo "  hexdump of readback:"
hexdump -C "$READBACK" 2>/dev/null || od -A x -t x1z "$READBACK"

if cmp -s "$EXPECTED" "$READBACK"; then
    pass "readback matches original payload"
else
    fail "readback DOES NOT match original payload"
    echo "  expected: $(cat "$EXPECTED")"
    echo "  got:      $(cat "$READBACK" 2>/dev/null)"
    PASS=0
fi

section "FT10.5: Cleanup"
del_obj "$TOKEN0" data "$LABEL"
rm -f "$READBACK" "$EXPECTED"

section "FT10: Summary"
if [ "$PASS" -eq 1 ]; then
    pass "FT10 PASSED"
    exit 0
else
    fail "FT10 FAILED"
    exit 1
fi
