#!/bin/sh
# ft13-target.sh - Persistence across tee-supplicant restart
# Deploy to /usr/bin/ft13-target.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  Persistence across tee-supplicant restart: write, restart daemon, read back."
    echo ""
    echo "Pass/Fail criteria: PASS if stored object is readable after tee-supplicant restart."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
TS_SECTOR=0x2000
TS_PAYLOAD="W77Q-TEESTORE-XVAL-SH001"
PKCS_PAYLOAD="W77Q-PKCS11-XVAL-SH001"
PKCS_LABEL="reread-test"
READBACK=/tmp/ft13_rb.bin

# Track 4 sub-checks
WRITE_TS=0
WRITE_PKCS=0
READ_TS=0
READ_PKCS=0

section "FT13: Persistence across tee-supplicant restart"

ensure_tee_supplicant

# --- WRITE PHASE ---
section "FT13.1: Write phase (before restart)"

echo "  tee-storage write-sector $TS_SECTOR ..."
tee-storage write-sector "$TS_SECTOR" "$TS_PAYLOAD" 2>&1
if [ $? -eq 0 ]; then
    pass "tee-storage write OK"
    WRITE_TS=1
else
    fail "tee-storage write failed"
fi

ensure_token "$TOKEN1" 1
del_obj "$TOKEN1" data "$PKCS_LABEL"

printf '%s' "$PKCS_PAYLOAD" > /tmp/ft13_expected.bin
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --write-object /tmp/ft13_expected.bin --type data \
    --label "$PKCS_LABEL" 2>&1
if [ $? -eq 0 ]; then
    pass "pkcs11 write OK"
    WRITE_PKCS=1
else
    fail "pkcs11 write failed"
fi

# --- RESTART ---
section "FT13.2: Restart tee-supplicant (simulates reboot LUT rebuild)"
echo "  restarting tee-supplicant..."
systemctl restart tee-supplicant
sleep 3
echo "  tee-supplicant status: $(systemctl is-active tee-supplicant)"

# --- READ PHASE ---
section "FT13.3: Read phase (after restart, LUT rebuilt from flash)"

echo "  tee-storage read-sector $TS_SECTOR ..."
TS_READ=$(tee-storage read-sector "$TS_SECTOR" 2>&1)
echo "  read: $(echo "$TS_READ" | head -3)"
TS_PREFIX=$(printf '%s' "$TS_PAYLOAD" | cut -c1-16)
if echo "$TS_READ" | grep -q "|${TS_PREFIX}"; then
    pass "tee-storage persisted across restart"
    READ_TS=1
else
    fail "tee-storage data lost after restart"
fi

echo "  pkcs11 read '$PKCS_LABEL' from $TOKEN1 ..."
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --read-object --type data \
    --label "$PKCS_LABEL" --output-file "$READBACK" 2>&1
if [ $? -eq 0 ] && cmp -s /tmp/ft13_expected.bin "$READBACK"; then
    pass "pkcs11 object persisted across restart"
    READ_PKCS=1
else
    fail "pkcs11 object lost or corrupted after restart"
fi

section "FT13.4: Cleanup"
tee-storage erase-sector "$TS_SECTOR" 2>/dev/null || true
del_obj "$TOKEN1" data "$PKCS_LABEL"
rm -f "$READBACK" /tmp/ft13_expected.bin

section "FT13: Summary (4 checks)"
echo "  write_ts=$WRITE_TS  write_pkcs=$WRITE_PKCS  read_ts=$READ_TS  read_pkcs=$READ_PKCS"
TOTAL=$((WRITE_TS + WRITE_PKCS + READ_TS + READ_PKCS))
echo "  $TOTAL/4 checks passed"
if [ "$TOTAL" -eq 4 ]; then
    pass "FT13 PASSED"
    exit 0
else
    fail "FT13 FAILED ($TOTAL/4)"
    exit 1
fi
