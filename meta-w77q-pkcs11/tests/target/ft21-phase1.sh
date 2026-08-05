#!/bin/sh
# ft21-phase1.sh - PKCS#11 persistent key creation, flash snapshot, then reboot
# Phase 1 of power-loss persistence test.
# Saves state to /root/ (persistent rootfs) before triggering reboot.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  Power-loss phase 1: create persistent RSA-2048 keypair, take flash snapshot, reboot."
    echo ""
    echo "Pass/Fail criteria: No verdict — sets up state for ft21-phase2.sh to verify."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
FT=FT21
KEY_LABEL=ft21-rsa-key
KEY_ID=21
MSG=/root/ft21_msg.bin
SIG=/root/ft21_sig.bin
PUB=/root/ft21_pub.der
BEFORE_LUT=/root/ft21_before_lut.txt
AFTER_LUT=/root/ft21_after_lut.txt

section "$FT Phase-1: Create persistent RSA key + snapshot + reboot"

ensure_tee_supplicant

# -----------------------------------------------------------------------
section "$FT.1: Flash snapshot BEFORE keygen"
w77q-dump list-all > "$BEFORE_LUT" 2>&1
echo "  LUT entries before: $(wc -l < "$BEFORE_LUT") lines"
cat "$BEFORE_LUT"

# -----------------------------------------------------------------------
section "$FT.2: Delete any stale ft21 key"
del_obj "$TOKEN1" privkey "$KEY_LABEL" 2>/dev/null || true
del_obj "$TOKEN1" pubkey  "$KEY_LABEL" 2>/dev/null || true
echo "  stale key cleanup done"

# -----------------------------------------------------------------------
section "$FT.3: Generate persistent RSA-2048 keypair (token object)"
ensure_token "$TOKEN1" 1
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --keypairgen --key-type rsa:2048 \
    --label "$KEY_LABEL" --id "$(printf '%02x' $KEY_ID)" \
    2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "RSA-2048 keypair created (token object)"
else
    fail "keypairgen failed (rc=$RC)"; exit 1
fi

# -----------------------------------------------------------------------
section "$FT.4: Export public key"
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --read-object --type pubkey \
    --label "$KEY_LABEL" --output-file "$PUB" 2>&1
if [ -s "$PUB" ]; then
    pass "public key exported: $(wc -c < "$PUB") bytes -> $PUB"
else
    fail "public key export failed"; exit 1
fi
# Convert to PEM for openssl
openssl rsa -pubin -inform DER -in "$PUB" -outform PEM \
    -out /root/ft21_pub.pem 2>/dev/null
echo "  PEM: $(wc -c < /root/ft21_pub.pem) bytes"

# -----------------------------------------------------------------------
section "$FT.5: Sign test message (pre-reboot)"
printf 'ft21-power-loss-test-message' > "$MSG"
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism SHA256-RSA-PKCS \
    --label "$KEY_LABEL" \
    --input-file "$MSG" --output-file "$SIG" 2>&1
RC=$?
if [ "$RC" -eq 0 ] && [ -s "$SIG" ]; then
    pass "signed OK: $(wc -c < "$SIG") byte signature"
    # verify immediately
    openssl dgst -sha256 -verify /root/ft21_pub.pem \
        -signature "$SIG" "$MSG" 2>&1 && pass "pre-reboot verify OK" \
        || fail "pre-reboot verify FAILED"
else
    fail "sign failed (rc=$RC)"; exit 1
fi

# -----------------------------------------------------------------------
section "$FT.6: Flash snapshot AFTER keygen"
w77q-dump list-all > "$AFTER_LUT" 2>&1
echo "  LUT entries after keygen: $(wc -l < "$AFTER_LUT") lines"
cat "$AFTER_LUT"
echo
NEW=$(grep -Fxvf "$BEFORE_LUT" "$AFTER_LUT" | grep 'flash_off' || true)
echo "  NEW flash entries:"
echo "${NEW:-  (none)}"

# Show raw hex of new entries
echo "$NEW" | while IFS= read -r nline; do
    [ -z "$nline" ] && continue
    OFF=$(echo "$nline" | grep -oE 'flash_off=[[:space:]]*0x[0-9a-fA-F]+' | grep -oE '0x[0-9a-fA-F]+')
    SZ=$(echo "$nline"  | grep -oE 'data_s[^=]*=[[:space:]]*[0-9]+' | grep -oE '[0-9]+$')
    [ -z "$OFF" ] && continue
    [ -z "$SZ"  ] && SZ=64
    echo "  --- raw @ $OFF ($SZ bytes) ---"
    w77q-dump read-raw "$OFF" "$SZ" 2>&1
done

# -----------------------------------------------------------------------
section "$FT.7: Marking phase-1 complete, rebooting..."
echo "PHASE1_OK" > /root/ft21_phase1_done.txt
echo "  Saved: msg=$MSG sig=$SIG pub=$PUB lut_before=$BEFORE_LUT lut_after=$AFTER_LUT"
echo "  Rebooting in 3 seconds..."
sleep 3
reboot
