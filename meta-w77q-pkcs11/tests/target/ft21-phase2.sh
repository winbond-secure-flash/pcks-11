#!/bin/sh
# ft21-phase2.sh - PKCS#11 power-loss persistence verification (post-reboot)
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  Power-loss phase 2: verify RSA keypair survived reboot, sign/verify, flash delta."
    echo ""
    echo "Pass/Fail criteria: PASS if keypair is intact and sign/verify succeeds post-reboot."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

PASS=1
FT=FT21
KEY_LABEL=ft21-rsa-key
MSG=/root/ft21_msg.bin
SIG_PRE=/root/ft21_sig.bin
SIG_POST=/tmp/ft21_sig_post.bin
PUB=/root/ft21_pub.pem
BEFORE_LUT=/root/ft21_before_lut.txt
AFTER_LUT=/root/ft21_after_lut.txt
POST_LUT=/tmp/ft21_post_lut.txt

section "$FT Phase-2: Verify persistent key survived power loss"

# -----------------------------------------------------------------------
section "$FT.8: Check phase-1 marker survived reboot"
if [ -f /root/ft21_phase1_done.txt ]; then
    pass "phase-1 marker present: $(cat /root/ft21_phase1_done.txt)"
else
    fail "/root/ft21_phase1_done.txt missing — phase-1 may not have completed"
fi

ensure_tee_supplicant

# -----------------------------------------------------------------------
section "$FT.9: Flash snapshot AFTER reboot"
w77q-dump list-all > "$POST_LUT" 2>&1
echo "  LUT entries post-reboot: $(wc -l < "$POST_LUT") lines"
cat "$POST_LUT"

echo
echo "  --- Comparison: AFTER-keygen vs POST-reboot ---"
NEW_AFTER_BOOT=$(grep -Fxvf "$AFTER_LUT" "$POST_LUT" | grep 'flash_off' || true)
GONE_AFTER_BOOT=$(grep -Fxvf "$POST_LUT" "$AFTER_LUT" | grep 'flash_off' || true)
echo "  NEW  since reboot: $(echo "$NEW_AFTER_BOOT"  | grep -c '[^[:space:]]' || true)"
echo "  GONE since reboot: $(echo "$GONE_AFTER_BOOT" | grep -c '[^[:space:]]' || true)"
if [ -n "$GONE_AFTER_BOOT" ]; then
    echo "  GONE entries (data loss!):"
    echo "$GONE_AFTER_BOOT"
fi

# -----------------------------------------------------------------------
section "$FT.10: Key object still present in PKCS#11 store"
KEY_LIST=$(pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --list-objects --type privkey 2>&1)
echo "$KEY_LIST"
if echo "$KEY_LIST" | grep -q "$KEY_LABEL"; then
    pass "private key '$KEY_LABEL' present after reboot"
else
    fail "private key '$KEY_LABEL' NOT FOUND after reboot"
fi

PUBKEY_LIST=$(pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --list-objects --type pubkey 2>&1)
echo "$PUBKEY_LIST"
if echo "$PUBKEY_LIST" | grep -q "$KEY_LABEL"; then
    pass "public key '$KEY_LABEL' present after reboot"
else
    fail "public key '$KEY_LABEL' NOT FOUND after reboot"
fi

# -----------------------------------------------------------------------
section "$FT.11: Pre-reboot signature still verifies with persisted key"
if [ -f "$SIG_PRE" ] && [ -f "$PUB" ] && [ -f "$MSG" ]; then
    openssl dgst -sha256 -verify "$PUB" -signature "$SIG_PRE" "$MSG" 2>&1
    if [ $? -eq 0 ]; then
        pass "pre-reboot signature verifies OK (key intact)"
    else
        fail "pre-reboot signature FAILED to verify"
    fi
else
    fail "missing files: sig=$SIG_PRE pub=$PUB msg=$MSG"
fi

# -----------------------------------------------------------------------
section "$FT.12: Sign new message POST-reboot with persisted key"
printf 'ft21-post-reboot-message' > /tmp/ft21_msg_post.bin
pkcs11-tool --module "$MOD" \
    --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism SHA256-RSA-PKCS \
    --label "$KEY_LABEL" \
    --input-file /tmp/ft21_msg_post.bin \
    --output-file "$SIG_POST" 2>&1
RC=$?
if [ "$RC" -eq 0 ] && [ -s "$SIG_POST" ]; then
    openssl dgst -sha256 -verify "$PUB" \
        -signature "$SIG_POST" /tmp/ft21_msg_post.bin 2>&1
    if [ $? -eq 0 ]; then
        pass "post-reboot sign + verify OK"
    else
        fail "post-reboot signature failed to verify"
    fi
else
    fail "post-reboot sign failed (rc=$RC)"
fi

# -----------------------------------------------------------------------
section "$FT.13: Flash delta: BEFORE-keygen vs POST-reboot (net change)"
echo "  Key entries added to flash (survive power loss):"
grep -Fxvf "$BEFORE_LUT" "$POST_LUT" | grep 'flash_off' || echo "  (none)"

# -----------------------------------------------------------------------
section "$FT.14: Cleanup"
del_obj "$TOKEN1" privkey "$KEY_LABEL" 2>/dev/null || true
del_obj "$TOKEN1" pubkey  "$KEY_LABEL" 2>/dev/null || true
rm -f /root/ft21_phase1_done.txt /root/ft21_pub.der \
      /root/ft21_pub.pem /root/ft21_msg.bin /root/ft21_sig.bin \
      /root/ft21_before_lut.txt /root/ft21_after_lut.txt \
      /tmp/ft21_post_lut.txt /tmp/ft21_sig_post.bin /tmp/ft21_msg_post.bin
echo "  cleanup done"

# -----------------------------------------------------------------------
section "$FT: Summary"
if [ "$PASS" -eq 1 ]; then
    pass "$FT PASSED — PKCS#11 private key survives power loss"
    exit 0
else
    fail "$FT FAILED"
    exit 1
fi
