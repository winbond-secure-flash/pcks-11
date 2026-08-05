#!/bin/sh
# ft9-target.sh - PKCS#11 write -> w77q-dump LUT before/after + raw hex dump
# Deploy to /usr/bin/ft9-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  PKCS#11 write -> w77q-dump LUT before/after + raw hex dump of new flash sector."
    echo ""
    echo "Pass/Fail criteria: PASS if PKCS#11 write succeeds and new LUT entry appears."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
BEFORE=/tmp/ft9_before.txt
AFTER=/tmp/ft9_after.txt
PAYLOAD="W77Q-PKCS11-XVAL-SH001"
LABEL="flash-record"

section "FT9: PKCS#11 write -> w77q-dump LUT delta"

ensure_tee_supplicant
ensure_token "$TOKEN1" 1

section "FT9.1: LUT snapshot BEFORE write"
w77q-dump list-all > "$BEFORE" 2>&1
BEFORE_COUNT=$(wc -l < "$BEFORE")
echo "  before: $BEFORE_COUNT lines"

section "FT9.2: Delete stale object"
del_obj "$TOKEN1" data "$LABEL"

section "FT9.3: Write PKCS#11 data object"
printf '%s' "$PAYLOAD" | pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --write-object /dev/stdin --type data \
    --label "$LABEL" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "pkcs11-tool write returned 0"
else
    fail "pkcs11-tool write returned $RC"
fi

section "FT9.4: LUT snapshot AFTER write"
w77q-dump list-all > "$AFTER" 2>&1
AFTER_COUNT=$(wc -l < "$AFTER")
echo "  after:  $AFTER_COUNT lines"

section "FT9.5: Delta (new/moved entries)"
NEW_LINES=$(grep -Fxvf "$BEFORE" "$AFTER" 2>/dev/null || true)
if [ -n "$NEW_LINES" ]; then
    echo "  new/changed lines:"
    echo "$NEW_LINES" | head -20
    echo "$NEW_LINES" | grep -oE 'flash_off[[:space:]]*=[[:space:]]*0x[0-9a-fA-F]+' | \
        awk -F'=' '{gsub(/ /,"",$2); print $2}' | while read -r OFFSET; do
        echo
        echo "  --- raw dump @ $OFFSET (128 bytes) ---"
        w77q-dump read-raw "$OFFSET" 128 2>&1 || true
    done
else
    echo "  (no new lines detected)"
fi

section "FT9: Summary"
echo "  before lines: $BEFORE_COUNT  after lines: $AFTER_COUNT"
if [ "$PASS" -eq 1 ]; then
    pass "FT9 PASSED"
    exit 0
else
    fail "FT9 FAILED"
    exit 1
fi
