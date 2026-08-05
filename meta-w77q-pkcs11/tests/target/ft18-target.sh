#!/bin/sh
# ft18-target.sh - pkcs11-demo RSA/EC/AES/RNG test + w77q-dump delta
# Deploy to /usr/bin/ft18-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  pkcs11-demo RSA/EC/AES/RNG algorithm test plus w77q-dump delta of flash changes."
    echo ""
    echo "Pass/Fail criteria: PASS if pkcs11-demo succeeds and flash delta is captured."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
BEFORE=/tmp/ft18_before.txt
AFTER=/tmp/ft18_after.txt
MAX_RAW_DUMPS=3

section "FT18: pkcs11-demo RSA/EC/AES/RNG + w77q-dump delta"

section "FT18.1: LUT snapshot BEFORE"
w77q-dump list-all > "$BEFORE" 2>&1
BEFORE_COUNT=$(wc -l < "$BEFORE")
echo "  before: $BEFORE_COUNT lines"

section "FT18.2: Run pkcs11-demo"
pkcs11-demo 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "pkcs11-demo returned 0"
else
    fail "pkcs11-demo returned $RC"
fi

section "FT18.3: LUT snapshot AFTER"
w77q-dump list-all > "$AFTER" 2>&1
AFTER_COUNT=$(wc -l < "$AFTER")
echo "  after:  $AFTER_COUNT lines"

section "FT18.4: Delta"
NEW_LINES=$(grep -Fxvf "$BEFORE" "$AFTER"  2>/dev/null || true)
GONE_LINES=$(grep -Fxvf "$AFTER"  "$BEFORE" 2>/dev/null || true)

NEW_COUNT=$(echo "$NEW_LINES"  | grep -c . 2>/dev/null || echo 0)
GONE_COUNT=$(echo "$GONE_LINES" | grep -c . 2>/dev/null || echo 0)
echo "  NEW: $NEW_COUNT  GONE: $GONE_COUNT"

if [ -n "$NEW_LINES" ]; then
    echo "  --- new/moved lines ---"
    echo "$NEW_LINES" | head -20
fi

section "FT18.5: Raw hex dumps of moved/new token.db entries (up to $MAX_RAW_DUMPS)"
DUMP_COUNT=0
echo "$NEW_LINES" | grep -oE 'flash_off[[:space:]]*=[[:space:]]*0x[0-9a-fA-F]+' | \
    awk -F'=' '{gsub(/ /,"",$2); print $2}' | \
    while read -r OFFSET; do
        if [ "$DUMP_COUNT" -ge "$MAX_RAW_DUMPS" ]; then
            break
        fi
        echo
        echo "  --- raw dump @ $OFFSET (128 bytes) ---"
        w77q-dump read-raw "$OFFSET" 128 2>&1 || true
        DUMP_COUNT=$((DUMP_COUNT + 1))
    done

section "FT18: Summary"
echo "  before: $BEFORE_COUNT  after: $AFTER_COUNT  delta: +$NEW_COUNT/-$GONE_COUNT"
if [ "$PASS" -eq 1 ]; then
    pass "FT18 PASSED"
    exit 0
else
    fail "FT18 FAILED"
    exit 1
fi
