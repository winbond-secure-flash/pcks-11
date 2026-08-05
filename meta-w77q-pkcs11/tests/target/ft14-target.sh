#!/bin/sh
# ft14-target.sh - Dump all LUT objects and sectors (diagnostic, no PASS/FAIL)
# Deploy to /usr/bin/ft14-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  Dump all LUT objects and sectors — diagnostic tool, no pass/fail verdict."
    echo ""
    echo "Pass/Fail criteria: None — informational dump only."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

section "FT14: Full diagnostic dump"

ensure_tee_supplicant

section "FT14.1: w77q-dump list-all"
w77q-dump list-all 2>&1 || echo "  (w77q-dump not available or no entries)"

section "FT14.2: tee-storage info + list"
tee-storage info 2>&1 || echo "  (tee-storage info not available)"
echo
tee-storage list 2>&1 || echo "  (tee-storage list not available)"

section "FT14.3: Scan sectors 0x0000..0xf000 (step 0x1000, non-empty only)"
ADDR=0
while [ $ADDR -le 61440 ]; do   # 0xf000 = 61440
    HEX=$(printf '0x%04x' $ADDR)
    RESULT=$(tee-storage read-sector "$HEX" 2>&1)
    if echo "$RESULT" | grep -qiv "empty\|error\|not found\|no data"; then
        if echo "$RESULT" | grep -qiE '[0-9]'; then
            echo "  $HEX: $RESULT"
        fi
    fi
    ADDR=$((ADDR + 4096))
done

section "FT14.4: pkcs11-tool list all slots"
pkcs11-tool --module "$MOD" -T 2>&1 || echo "  (pkcs11-tool not available)"

section "FT14.5: List objects in each initialized slot"
SLOTS=$(pkcs11-tool --module "$MOD" -T 2>&1 | \
    grep -E 'Slot [0-9]+' | awk '{print $2}' || true)
if [ -z "$SLOTS" ]; then
    echo "  (no slots found)"
fi
for SLOT in $SLOTS; do
    echo
    echo "  -- Slot $SLOT objects --"
    pkcs11-tool --module "$MOD" --slot "$SLOT" \
        -O --login --pin "$UPIN" 2>&1 || \
        echo "    (login failed or no objects)"
done

section "FT14: Done (diagnostic only)"
