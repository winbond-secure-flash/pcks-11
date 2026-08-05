#!/bin/sh
# ft20-target.sh - Full flash state compare: before/after pkcs11-demo
# Captures LUT + raw content of every entry, runs pkcs11-demo, then diffs.
# Deploy to /usr/bin/ft20-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  Full flash state compare: captures LUT before pkcs11-demo, runs it, then diffs."
    echo ""
    echo "Pass/Fail criteria: PASS if pkcs11-demo succeeds and flash diff is captured cleanly."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
FT=FT20
BEFORE_LUT=/tmp/ft20_before_lut.txt
AFTER_LUT=/tmp/ft20_after_lut.txt
BEFORE_DATA=/tmp/ft20_before_data.txt
AFTER_DATA=/tmp/ft20_after_data.txt

# dump_raw_entries <lut_file> <data_file>
# For each LUT entry in lut_file, appends "=== ENTRY ===" + raw hex dump to data_file.
dump_raw_entries() {
    local lut="$1" data="$2"
    > "$data"
    while IFS= read -r line; do
        echo "$line" | grep -q 'flash_off' || continue
        OFF=$(echo "$line" | grep -oE 'flash_off=[[:space:]]*0x[0-9a-fA-F]+' \
                           | grep -oE '0x[0-9a-fA-F]+')
        SZ=$(echo "$line"  | grep -oE 'data_s[^=]*=[[:space:]]*[0-9]+' \
                           | grep -oE '[0-9]+$')
        [ -z "$OFF" ] && continue
        [ -z "$SZ"  ] && SZ=64
        [ "$SZ" -eq 0 ] && SZ=64
        printf '=== %s ===\n' "$line" >> "$data"
        w77q-dump read-raw "$OFF" "$SZ" >> "$data" 2>&1
        printf '\n' >> "$data"
    done < "$lut"
}

section "$FT: Full flash compare around pkcs11-demo"
ensure_tee_supplicant

# ---------------------------------------------------------------------------
section "$FT.1: Full flash snapshot BEFORE pkcs11-demo"
w77q-dump list-all > "$BEFORE_LUT" 2>&1
BEFORE_LUT_COUNT=$(wc -l < "$BEFORE_LUT")
echo "  LUT entries before: $BEFORE_LUT_COUNT lines"
cat "$BEFORE_LUT"
echo
echo "  Dumping raw content of each LUT entry..."
dump_raw_entries "$BEFORE_LUT" "$BEFORE_DATA"
echo "  Raw dump: $(wc -l < "$BEFORE_DATA") lines"

# ---------------------------------------------------------------------------
section "$FT.2: Run pkcs11-demo"
pkcs11-demo 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "pkcs11-demo returned 0"
else
    fail "pkcs11-demo returned $RC"
fi

# ---------------------------------------------------------------------------
section "$FT.3: Full flash snapshot AFTER pkcs11-demo"
w77q-dump list-all > "$AFTER_LUT" 2>&1
AFTER_LUT_COUNT=$(wc -l < "$AFTER_LUT")
echo "  LUT entries after: $AFTER_LUT_COUNT lines"
cat "$AFTER_LUT"
echo
echo "  Dumping raw content of each LUT entry..."
dump_raw_entries "$AFTER_LUT" "$AFTER_DATA"
echo "  Raw dump: $(wc -l < "$AFTER_DATA") lines"

# ---------------------------------------------------------------------------
section "$FT.4: LUT delta (NEW / GONE entries)"

NEW_LINES=$(grep -Fxvf "$BEFORE_LUT" "$AFTER_LUT" 2>/dev/null \
            | grep 'flash_off' || true)
GONE_LINES=$(grep -Fxvf "$AFTER_LUT"  "$BEFORE_LUT" 2>/dev/null \
             | grep 'flash_off' || true)

NEW_COUNT=$(echo "$NEW_LINES"  | grep -c '[^[:space:]]' 2>/dev/null || true)
GONE_COUNT=$(echo "$GONE_LINES" | grep -c '[^[:space:]]' 2>/dev/null || true)
NEW_COUNT=${NEW_COUNT:-0}
GONE_COUNT=${GONE_COUNT:-0}

echo "  LUT delta: NEW=$NEW_COUNT  GONE=$GONE_COUNT"

if [ "$NEW_COUNT" -gt 0 ]; then
    echo
    echo "  --- NEW entries (written by pkcs11-demo) ---"
    echo "$NEW_LINES"
fi
if [ "$GONE_COUNT" -gt 0 ]; then
    echo
    echo "  --- GONE entries (invalidated / wear-levelled) ---"
    echo "$GONE_LINES"
fi

# ---------------------------------------------------------------------------
section "$FT.5: Raw content diff (byte-level changes)"

# Strip the "=== [NNN] ... flash_off=0xXXX ... ===" header lines before diff
# so we compare data bytes independently of where they were stored.
grep -v '^===' "$BEFORE_DATA" > /tmp/ft20_before_stripped.txt
grep -v '^===' "$AFTER_DATA"  > /tmp/ft20_after_stripped.txt

DIFF_OUT=$(diff /tmp/ft20_before_stripped.txt /tmp/ft20_after_stripped.txt 2>/dev/null | head -120)
if [ -z "$DIFF_OUT" ]; then
    echo "  Byte content: IDENTICAL (no changes to existing entries)"
    pass "existing entry content unchanged"
else
    echo "  Byte content CHANGED (< = before, > = after):"
    echo "$DIFF_OUT"
fi

# ---------------------------------------------------------------------------
section "$FT.6: Full raw dump of NEW entries"
if [ "$NEW_COUNT" -gt 0 ]; then
    echo "$NEW_LINES" | while IFS= read -r nline; do
        [ -z "$nline" ] && continue
        OFF=$(echo "$nline" | grep -oE 'flash_off=[[:space:]]*0x[0-9a-fA-F]+' \
                            | grep -oE '0x[0-9a-fA-F]+')
        SZ=$(echo "$nline"  | grep -oE 'data_s[^=]*=[[:space:]]*[0-9]+' \
                            | grep -oE '[0-9]+$')
        [ -z "$OFF" ] && continue
        [ -z "$SZ"  ] && SZ=64
        [ "$SZ" -eq 0 ] && SZ=64
        echo "  --- $nline ---"
        w77q-dump read-raw "$OFF" "$SZ" 2>&1
        echo
    done
else
    echo "  No new entries to dump."
fi

# ---------------------------------------------------------------------------
# Cleanup
rm -f /tmp/ft20_before_stripped.txt /tmp/ft20_after_stripped.txt

section "$FT: Summary"
if [ "$PASS" -eq 1 ]; then
    pass "$FT PASSED"
    exit 0
else
    fail "$FT FAILED"
    exit 1
fi
