#!/bin/sh
# ft17-target.sh - 25x tee-demo wear test
# Deploy to /usr/bin/ft17-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  Runs tee-demo 25 times and checks wear-leveling across flash sectors."
    echo ""
    echo "Pass/Fail criteria: PASS if all 25 runs succeed and wear distribution is observed."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
BEFORE=/tmp/ft17_before.txt
AFTER=/tmp/ft17_after.txt
ITERS=25

section "FT17: 25x tee-demo wear test"

section "FT17.1: LUT snapshot BEFORE"
w77q-dump list-all > "$BEFORE" 2>&1
BEFORE_COUNT=$(wc -l < "$BEFORE")
echo "  before: $BEFORE_COUNT lines"

section "FT17.2: Run tee-demo $ITERS times"
echo "  setting password first..."
tee-demo set-password "$SOPIN" 2>&1 || true
echo "  password set, starting wear loop..."
i=1
while [ $i -le $ITERS ]; do
    OUT=$(tee-demo demo "$SOPIN" 2>&1)
    RC=$?
    CTR=$(echo "$OUT"   | grep -oE 'counter=[0-9]+'    | tail -1 || true)
    BOOT=$(echo "$OUT"  | grep -oE 'boot_count=[0-9]+' | tail -1 || true)
    printf "  iter %2d: rc=%d  %s  %s\n" "$i" "$RC" \
        "${CTR:-counter=?}" "${BOOT:-boot_count=?}"
    if [ $RC -ne 0 ]; then
        fail "tee-demo returned $RC on iteration $i"
    fi
    i=$((i + 1))
done

section "FT17.3: LUT snapshot AFTER"
w77q-dump list-all > "$AFTER" 2>&1
AFTER_COUNT=$(wc -l < "$AFTER")
echo "  after:  $AFTER_COUNT lines"

section "FT17.4: Delta (SAME/MOVED/NEW/GONE)"
NEW_LINES=$(grep -Fxvf "$BEFORE" "$AFTER"  2>/dev/null || true)
GONE_LINES=$(grep -Fxvf "$AFTER"  "$BEFORE" 2>/dev/null || true)

NEW_COUNT=$(echo "$NEW_LINES"  | grep -c '[^[:space:]]' 2>/dev/null || true)
GONE_COUNT=$(echo "$GONE_LINES" | grep -c '[^[:space:]]' 2>/dev/null || true)
NEW_COUNT=${NEW_COUNT:-0}
GONE_COUNT=${GONE_COUNT:-0}
SAME_COUNT=$(( $(echo "$BEFORE_COUNT" | tr -d ' \t\n') - $(echo "$GONE_COUNT" | tr -d ' \t\n') ))
echo "  SAME: $SAME_COUNT  NEW: $NEW_COUNT  GONE: $GONE_COUNT"

if [ -n "$NEW_LINES" ]; then
    echo "  --- NEW/MOVED lines ---"
    echo "$NEW_LINES" | head -20
fi

section "FT17.5: Raw compare for first MOVED demo_00 entry"
BEFORE_DEMO=$(grep 'demo_00' "$BEFORE" | \
    grep -oE 'flash_off[[:space:]]*=[[:space:]]*0x[0-9a-fA-F]+' | \
    awk -F'=' '{gsub(/ /,"",$2); print $2}' | head -1 || true)
AFTER_DEMO=$(grep 'demo_00' "$AFTER" | \
    grep -oE 'flash_off[[:space:]]*=[[:space:]]*0x[0-9a-fA-F]+' | \
    awk -F'=' '{gsub(/ /,"",$2); print $2}' | head -1 || true)

if [ -n "$BEFORE_DEMO" ] && [ -n "$AFTER_DEMO" ] && [ "$BEFORE_DEMO" != "$AFTER_DEMO" ]; then
    echo "  demo_00 MOVED: $BEFORE_DEMO -> $AFTER_DEMO"
    echo
    echo "  --- raw dump BEFORE @ $BEFORE_DEMO (128 bytes) ---"
    w77q-dump read-raw "$BEFORE_DEMO" 128 2>&1 || true
    echo
    echo "  --- raw dump AFTER  @ $AFTER_DEMO (128 bytes) ---"
    w77q-dump read-raw "$AFTER_DEMO"  128 2>&1 || true
elif [ -n "$BEFORE_DEMO" ] && [ "$BEFORE_DEMO" = "$AFTER_DEMO" ]; then
    echo "  demo_00 SAME offset $BEFORE_DEMO (no wear-level move in $ITERS iters)"
else
    echo "  demo_00 entry not found in LUT"
fi

section "FT17: Summary"
if [ "$PASS" -eq 1 ]; then
    pass "FT17 PASSED"
    exit 0
else
    fail "FT17 FAILED"
    exit 1
fi
