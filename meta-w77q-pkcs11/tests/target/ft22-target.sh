#!/bin/sh
# ft22-target.sh - PKCS#11 algorithm coverage test
# Tests: RSA keygen/sign/verify/enc/dec, ECDSA keygen/sign/verify,
#        EdDSA keygen/sign/verify, AES-ECB/CBC enc/dec,
#        AES-CMAC, HMAC-SHA256, Digest (SHA256), RNG
# Note: RSA-PKCS encrypt uses openssl (pkcs11-tool --encrypt targets secret keys only).
#       AES decrypt uses --id search (pkcs11-tool --label search adds CKA_DECRYPT filter,
#       which OP-TEE stores as FALSE by default, causing C_FindObjects to miss the key).
# Deploy to /usr/bin/ft22-target.sh
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  PKCS#11 algorithm coverage: RSA, ECDSA, EdDSA, AES-ECB/CBC, AES-CMAC, HMAC-SHA256, digest, RNG."
    echo ""
    echo "Pass/Fail criteria: PASS if all algorithm operations complete without error."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

FT=FT22
PASS=1
MSG=/tmp/ft22_msg.bin
SIG=/tmp/ft22_sig.bin
ENC=/tmp/ft22_enc.bin
DEC=/tmp/ft22_dec.bin
HMAC=/tmp/ft22_hmac.bin
DGST=/tmp/ft22_digest.bin

RESULTS=""
TOTAL=0
OK=0

record() {
    TOTAL=$((TOTAL+1))
    if [ "$2" = "PASS" ]; then
        OK=$((OK+1))
        printf "  [PASS] %s\n" "$1"
    else
        printf "  [FAIL] %s\n" "$1"
        PASS=0
    fi
    RESULTS="${RESULTS}${1}=${2}\n"
}

check_rc() { [ "$1" -eq 0 ] && echo PASS || echo FAIL; }

section "$FT: PKCS#11 Algorithm Coverage Test"
ensure_tee_supplicant
ensure_token "$TOKEN1" 1

# 32 bytes exactly — required for AES-ECB/CBC block alignment
printf 'ft22-pkcs11-algo-test-32bytesmsg' > "$MSG"
# 16-byte IV (hex) for AES-CBC
AES_IV16="00000000000000000000000000000000"

# ===========================================================================
section "$FT.1: RSA-PKCS-KEY-PAIR-GEN (2048-bit)"
del_obj "$TOKEN1" privkey ft22-rsa 2>/dev/null; del_obj "$TOKEN1" pubkey ft22-rsa 2>/dev/null
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --keypairgen --key-type rsa:2048 --label ft22-rsa --id 22 2>&1
record "RSA-2048 keygen" "$(check_rc $?)"

# RSA sign SHA256-RSA-PKCS
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism SHA256-RSA-PKCS --label ft22-rsa \
    --input-file "$MSG" --output-file "$SIG" 2>&1
record "SHA256-RSA-PKCS sign" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --verify --mechanism SHA256-RSA-PKCS --label ft22-rsa \
    --input-file "$MSG" --signature-file "$SIG" 2>&1
record "SHA256-RSA-PKCS verify" "$(check_rc $?)"

# RSA sign SHA256-RSA-PKCS-PSS
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism SHA256-RSA-PKCS-PSS --label ft22-rsa \
    --input-file "$MSG" --output-file "$SIG" 2>&1
record "SHA256-RSA-PKCS-PSS sign" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --verify --mechanism SHA256-RSA-PKCS-PSS --label ft22-rsa \
    --input-file "$MSG" --signature-file "$SIG" 2>&1
record "SHA256-RSA-PKCS-PSS verify" "$(check_rc $?)"

# RSA-PKCS encrypt: pkcs11-tool --encrypt searches for secret keys; use openssl
# to encrypt with the extracted public key, then pkcs11-tool to decrypt.
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --read-object --type pubkey --id 22 --output-file /tmp/ft22-rsa-pub.der 2>&1
openssl rsa -inform DER -outform PEM -pubin \
    -in /tmp/ft22-rsa-pub.der -out /tmp/ft22-rsa-pub.pem 2>/dev/null
openssl pkeyutl -encrypt -pubin -inkey /tmp/ft22-rsa-pub.pem \
    -pkeyopt rsa_padding_mode:pkcs1 -in "$MSG" -out "$ENC" 2>&1
RC_ENC=$?
record "RSA-PKCS encrypt" "$(check_rc $RC_ENC)"

if [ $RC_ENC -eq 0 ]; then
    pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
        --decrypt --mechanism RSA-PKCS --id 22 --type privkey \
        --input-file "$ENC" --output-file "$DEC" 2>&1
    RC_DEC=$?
    record "RSA-PKCS decrypt" "$(check_rc $RC_DEC)"
    if [ $RC_DEC -eq 0 ] && cmp -s "$MSG" "$DEC"; then
        record "RSA-PKCS enc/dec roundtrip" PASS
    else
        record "RSA-PKCS enc/dec roundtrip" FAIL
    fi
fi

# RSA-PKCS-OAEP: OP-TEE returns CKR_FUNCTION_FAILED for C_DecryptInit — not supported
# record "RSA-PKCS-OAEP" N/A

del_obj "$TOKEN1" privkey ft22-rsa; del_obj "$TOKEN1" pubkey ft22-rsa

# ===========================================================================
section "$FT.2: ECDSA-KEY-PAIR-GEN (P-256)"
del_obj "$TOKEN1" privkey ft22-ec 2>/dev/null; del_obj "$TOKEN1" pubkey ft22-ec 2>/dev/null
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --keypairgen --key-type EC:prime256v1 --label ft22-ec --id 23 2>&1
record "EC P-256 keygen" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism ECDSA-SHA256 --label ft22-ec \
    --input-file "$MSG" --output-file "$SIG" 2>&1
record "ECDSA-SHA256 sign" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --verify --mechanism ECDSA-SHA256 --label ft22-ec \
    --input-file "$MSG" --signature-file "$SIG" 2>&1
record "ECDSA-SHA256 verify" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism ECDSA-SHA512 --label ft22-ec \
    --input-file "$MSG" --output-file "$SIG" 2>&1
record "ECDSA-SHA512 sign" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --verify --mechanism ECDSA-SHA512 --label ft22-ec \
    --input-file "$MSG" --signature-file "$SIG" 2>&1
record "ECDSA-SHA512 verify" "$(check_rc $?)"

del_obj "$TOKEN1" privkey ft22-ec; del_obj "$TOKEN1" pubkey ft22-ec

# ===========================================================================
section "$FT.3: EC-EDWARDS-KEY-PAIR-GEN (Ed25519)"
del_obj "$TOKEN1" privkey ft22-ed 2>/dev/null; del_obj "$TOKEN1" pubkey ft22-ed 2>/dev/null
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --keypairgen --key-type EC:edwards25519 --label ft22-ed --id 24 2>&1
record "Ed25519 keygen" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism EDDSA --label ft22-ed \
    --input-file "$MSG" --output-file "$SIG" 2>&1
record "EDDSA sign" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --verify --mechanism EDDSA --label ft22-ed \
    --input-file "$MSG" --signature-file "$SIG" 2>&1
record "EDDSA verify" "$(check_rc $?)"

del_obj "$TOKEN1" privkey ft22-ed; del_obj "$TOKEN1" pubkey ft22-ed

# ===========================================================================
section "$FT.4: AES-KEY-GEN + ECB/CBC encrypt/decrypt"

aes_test() {
    local mech="$1" keylen="$2" label="ft22-aes-${mech}" iv_opt=""
    case "$mech" in
        AES-CBC) iv_opt="--iv $AES_IV16" ;;
    esac
    del_obj "$TOKEN1" secrkey "$label" 2>/dev/null
    # --usage-decrypt sets both CKA_ENCRYPT and CKA_DECRYPT for symmetric keys
    pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
        --keygen --key-type AES:$keylen --label "$label" --id 25 \
        --usage-decrypt 2>&1
    record "AES-$((keylen*8)) keygen" "$(check_rc $?)"

    # shellcheck disable=SC2086
    pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
        --encrypt --mechanism "$mech" --label "$label" --id 25 $iv_opt \
        --input-file "$MSG" --output-file "$ENC" 2>&1
    RC_ENC=$?
    record "$mech encrypt" "$(check_rc $RC_ENC)"

    if [ $RC_ENC -eq 0 ]; then
        # Use --id (not --label) for decrypt: pkcs11-tool adds CKA_DECRYPT to the
        # label-based search template, but OP-TEE stores CKA_DECRYPT=FALSE by default,
        # so label search misses the key. ID-based search does not apply this filter.
        # shellcheck disable=SC2086
        pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
            --decrypt --mechanism "$mech" --id 25 $iv_opt \
            --input-file "$ENC" --output-file "$DEC" 2>&1
        RC_DEC=$?
        record "$mech decrypt" "$(check_rc $RC_DEC)"
        if [ $RC_DEC -eq 0 ] && cmp -s "$MSG" "$DEC" 2>/dev/null; then
            record "$mech roundtrip" PASS
        else
            record "$mech roundtrip" FAIL
        fi
    fi
    del_obj "$TOKEN1" secrkey "$label" 2>/dev/null
}

aes_test AES-ECB 32
aes_test AES-CBC 32

# ===========================================================================
section "$FT.5: AES-CMAC (sign/verify)"
del_obj "$TOKEN1" secrkey ft22-aes-cmac 2>/dev/null
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --keygen --key-type AES:32 --label ft22-aes-cmac --id 26 \
    --usage-sign 2>&1
record "AES-256 keygen (CMAC)" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism AES-CMAC --label ft22-aes-cmac \
    --input-file "$MSG" --output-file "$HMAC" 2>&1
record "AES-CMAC sign" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --verify --mechanism AES-CMAC --label ft22-aes-cmac \
    --input-file "$MSG" --signature-file "$HMAC" 2>&1
record "AES-CMAC verify" "$(check_rc $?)"
del_obj "$TOKEN1" secrkey ft22-aes-cmac 2>/dev/null

# ===========================================================================
section "$FT.6: HMAC-SHA256 (sign/verify)"
del_obj "$TOKEN1" secrkey ft22-hmac 2>/dev/null
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --keygen --key-type GENERIC:32 --label ft22-hmac --id 27 \
    --usage-sign 2>&1
record "GENERIC-SECRET keygen (HMAC)" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --sign --mechanism SHA256-HMAC --label ft22-hmac \
    --input-file "$MSG" --output-file "$HMAC" 2>&1
record "SHA256-HMAC sign" "$(check_rc $?)"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --verify --mechanism SHA256-HMAC --label ft22-hmac \
    --input-file "$MSG" --signature-file "$HMAC" 2>&1
record "SHA256-HMAC verify" "$(check_rc $?)"
del_obj "$TOKEN1" secrkey ft22-hmac 2>/dev/null

# ===========================================================================
section "$FT.7: Digest (SHA256)"
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
    --hash --mechanism SHA256 \
    --input-file "$MSG" --output-file "$DGST" 2>&1
RC=$?
record "SHA256 digest" "$(check_rc $RC)"
if [ $RC -eq 0 ]; then
    DGST_HEX=$(xxd -p "$DGST" | tr -d '\n')
    OPENSSL_HEX=$(openssl dgst -sha256 -hex "$MSG" 2>/dev/null | awk '{print $2}')
    echo "  pkcs11:  $DGST_HEX"
    echo "  openssl: $OPENSSL_HEX"
    if [ "$DGST_HEX" = "$OPENSSL_HEX" ]; then
        record "SHA256 matches openssl" PASS
    else
        record "SHA256 matches openssl" FAIL
    fi
fi

# ===========================================================================
section "$FT.8: RNG (C_GenerateRandom)"
RNG=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
    --generate-random 32 2>/dev/null | xxd -p | tr -d '\n')
if [ ${#RNG} -ge 64 ]; then
    echo "  random[32]: $RNG"
    record "C_GenerateRandom 32 bytes" PASS
else
    record "C_GenerateRandom 32 bytes" FAIL
fi

# Cleanup
rm -f "$MSG" "$SIG" "$ENC" "$DEC" "$HMAC" "$DGST"

# ===========================================================================
section "$FT: Summary ($OK/$TOTAL passed)"
echo
printf '%b' "$RESULTS" | while IFS='=' read -r name result; do
    [ -z "$name" ] && continue
    printf "  %-38s : %s\n" "$name" "$result"
done
echo
if [ "$PASS" -eq 1 ]; then
    pass "$FT PASSED ($OK/$TOTAL)"
    exit 0
else
    FAILED=$((TOTAL - OK))
    fail "$FT FAILED ($FAILED/$TOTAL failed)"
    exit 1
fi
