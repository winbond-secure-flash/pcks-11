#!/bin/sh
# ft16-target.sh - PKCS#11 write -> w77q-dump delta (detailed, mirrors ft16.py)
# Deploy to /usr/bin/ft16-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  PKCS#11 write then detailed w77q-dump delta showing new/changed flash sectors."
    echo ""
    echo "Pass/Fail criteria: PASS if PKCS#11 write succeeds and delta sector is identified."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
BEFORE=/tmp/ft16_before.txt
AFTER=/tmp/ft16_after.txt
PAYLOAD="SPARROW-HAWK-DUMP-DEMO-2026"
LABEL="dump-test-obj"
OBJ_ID=42

section "FT16: PKCS#11 write -> w77q-dump delta (detailed)"

ensure_tee_supplicant

section "FT16.1: LUT snapshot BEFORE"
w77q-dump list-all > "$BEFORE" 2>&1
BEFORE_COUNT=$(wc -l < "$BEFORE")
echo "  before: $BEFORE_COUNT lines"

section "FT16.2: Prepare token and delete stale object"
ensure_token "$TOKEN0" 0
del_obj "$TOKEN0" data "$LABEL"

section "FT16.3: Write PKCS#11 data object (id=$OBJ_ID)"
printf '%s' "$PAYLOAD" > /tmp/ft16_expected.bin
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN0" --login --pin "$UPIN" \
    --write-object /tmp/ft16_expected.bin --type data \
    --label "$LABEL" --id "$(printf '%02x' $OBJ_ID)" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "pkcs11 write OK"
else
    fail "pkcs11 write returned $RC"
fi

section "FT16.4: LUT snapshot AFTER"
w77q-dump list-all > "$AFTER" 2>&1
AFTER_COUNT=$(wc -l < "$AFTER")
echo "  after:  $AFTER_COUNT lines"

section "FT16.5: Delta analysis"
NEW_LINES=$(grep -Fxvf "$BEFORE" "$AFTER" 2>/dev/null || true)
GONE_LINES=$(grep -Fxvf "$AFTER" "$BEFORE" 2>/dev/null || true)

NEW_COUNT=$(echo "$NEW_LINES" | grep -c . 2>/dev/null || echo 0)
GONE_COUNT=$(echo "$GONE_LINES" | grep -c . 2>/dev/null || echo 0)
echo "  NEW entries:  $NEW_COUNT"
echo "  GONE entries: $GONE_COUNT"

if [ -n "$NEW_LINES" ]; then
    echo "  --- new/moved lines ---"
    echo "$NEW_LINES"
fi
if [ -n "$GONE_LINES" ]; then
    echo "  --- gone lines ---"
    echo "$GONE_LINES"
fi

section "FT16.6: Raw hex dump of new/moved entries"
echo "$NEW_LINES" | grep -oE 'flash_off[[:space:]]*=[[:space:]]*0x[0-9a-fA-F]+' | \
    awk -F'=' '{gsub(/ /,"",$2); print $2}' | while read -r OFFSET; do
    echo
    echo "  --- raw dump @ $OFFSET (128 bytes) ---"
    w77q-dump read-raw "$OFFSET" 128 2>&1 || true
done

section "FT16.7: Cleanup"
del_obj "$TOKEN0" data "$LABEL"
rm -f /tmp/ft16_expected.bin

section "FT16: Summary"
echo "  before lines: $BEFORE_COUNT  after lines: $AFTER_COUNT"
echo "  delta: +$NEW_COUNT new / -$GONE_COUNT gone"
if [ "$PASS" -eq 1 ]; then
    pass "FT16 PASSED"
    exit 0
else
    fail "FT16 FAILED"
    exit 1
fi
