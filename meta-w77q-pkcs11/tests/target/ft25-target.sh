#!/bin/sh
# ft25-target.sh — RSA Keygen + Encrypt with Public Key + Decrypt with Private Key
#
# FT25.1 — RSA-2048 key pair generation (privkey + pubkey in TOKEN1)
# FT25.2 — Extract public key DER from token
# FT25.3 — Encrypt plaintext with public key (openssl pkeyutl / RSA-PKCS1)
# FT25.4 — Decrypt ciphertext with private key (PKCS#11 C_Decrypt / RSA-PKCS)
# FT25.5 — Roundtrip: decrypted == original plaintext (byte-exact compare)
# FT25.6 — PSS variant: encrypt with OAEP is not supported; sign+verify instead
#           (demonstrates private key stays non-extractable, CKA_SENSITIVE=TRUE)
#
# Note: pkcs11-tool --encrypt targets CKO_SECRET_KEY only (limitation of v0.25.1).
#       Encryption with RSA public key is done via openssl using the exported DER.
#       Decryption is done via pkcs11-tool --decrypt with the private key in the TEE.
#
# Deploy to /usr/bin/ft25-target.sh
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  RSA-2048 keygen + encrypt (public key) + decrypt (private key):"
    echo "    FT25.1  RSA-2048 key pair generation"
    echo "    FT25.2  Export public key DER from token"
    echo "    FT25.3  Encrypt plaintext with public key (openssl RSA-PKCS1)"
    echo "    FT25.4  Decrypt ciphertext with private key (PKCS#11 RSA-PKCS)"
    echo "    FT25.5  Roundtrip integrity: decrypted == original plaintext"
    echo "    FT25.6  Private key non-extractable (CKA_SENSITIVE=TRUE)"
    echo ""
    echo "Pass/Fail criteria: PASS if all encrypt/decrypt operations succeed"
    echo "  and decrypted output matches plaintext byte-for-byte."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

FT=FT25
PASS=1
TOTAL=0
OK=0
RESULTS=""

PLAIN=/tmp/ft25_plain.bin
ENC=/tmp/ft25_enc.bin
DEC=/tmp/ft25_dec.bin
PUB_DER=/tmp/ft25_pub.der
PUB_PEM=/tmp/ft25_pub.pem
KEYMATERIAL=/tmp/ft25_keymaterial.bin
KEYMATERIAL_ENC=/tmp/ft25_keymaterial_enc.bin
KEYMATERIAL_DEC=/tmp/ft25_keymaterial_dec.bin

record() {
    TOTAL=$((TOTAL+1))
    if [ "$2" = "PASS" ]; then
        OK=$((OK+1))
        printf "  [PASS] %s\n" "$1"
    else
        printf "  [FAIL] %s%s\n" "$1" "${3:+  ($3)}"
        PASS=0
    fi
    RESULTS="${RESULTS}${1}=${2}\n"
}

check_rc() { [ "$1" -eq 0 ] && echo PASS || echo FAIL; }

section "$FT: RSA Keygen + Encrypt (pubkey) + Decrypt (privkey)"
ensure_tee_supplicant
ensure_token "$TOKEN1" 1

# Random plaintext: 1 KB – 10 MB (use 2 urandom bytes for range up to 10240 KB)
RAND_KB=$(( $(od -A n -N2 -t u2 /dev/urandom | tr -d ' ') % 10240 + 1 ))
dd if=/dev/urandom bs=1024 count="$RAND_KB" of="$PLAIN" 2>/dev/null
PLAIN_LEN=$(wc -c < "$PLAIN")
echo "  random plaintext: $PLAIN_LEN bytes (${RAND_KB} KB)"

# Stale cleanup
del_obj "$TOKEN1" privkey ft25-rsa 2>/dev/null
del_obj "$TOKEN1" pubkey  ft25-rsa 2>/dev/null

# ---------------------------------------------------------------------------
# FT25.1 — Key pair generation
# ---------------------------------------------------------------------------
section "$FT.1: RSA-2048 key pair generation"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --keypairgen --key-type rsa:2048 --label ft25-rsa --id 25 2>&1
record "RSA-2048 keygen" "$(check_rc $?)"

# ---------------------------------------------------------------------------
# FT25.2 — Export public key DER
# ---------------------------------------------------------------------------
section "$FT.2: Export public key DER from token"

pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --read-object --type pubkey --id 25 --output-file "$PUB_DER" 2>&1
RC_DER=$?
record "read pubkey DER" "$(check_rc $RC_DER)"

if [ "$RC_DER" -eq 0 ]; then
    openssl rsa -inform DER -outform PEM -pubin \
        -in "$PUB_DER" -out "$PUB_PEM" 2>/dev/null
    record "convert DER -> PEM" "$(check_rc $?)"
    echo "  public key modulus (first line of PEM):"
    head -2 "$PUB_PEM"
else
    record "convert DER -> PEM" FAIL "pubkey DER read failed"
fi

# ---------------------------------------------------------------------------
# FT25.3 — Hybrid encrypt: RSA wraps AES key, AES encrypts data
# ---------------------------------------------------------------------------
section "$FT.3: Hybrid encrypt (RSA wraps AES-256 key, AES-CBC encrypts data)"

RC_ENC=1
if [ -f "$PUB_PEM" ]; then
    # Generate 48 bytes: 32-byte AES-256 key + 16-byte IV
    openssl rand -out "$KEYMATERIAL" 48 2>&1
    AES_KEY_HEX=$(od -A n -t x1 "$KEYMATERIAL" | tr -d ' \n' | cut -c1-64)
    AES_IV_HEX=$(od  -A n -t x1 "$KEYMATERIAL" | tr -d ' \n' | cut -c65-96)
    record "generate AES-256 key + IV (48 B)" "$(check_rc $?)"

    # Wrap key material with RSA public key (48 B << 245 B PKCS1 max)
    openssl pkeyutl -encrypt -pubin -inkey "$PUB_PEM" \
        -pkeyopt rsa_padding_mode:pkcs1 \
        -in "$KEYMATERIAL" -out "$KEYMATERIAL_ENC" 2>&1
    record "RSA-wrap AES key with public key" "$(check_rc $?)"

    # Encrypt data with AES-256-CBC
    openssl enc -aes-256-cbc -K "$AES_KEY_HEX" -iv "$AES_IV_HEX" \
        -nosalt -in "$PLAIN" -out "$ENC" 2>&1
    RC_ENC=$?
    ENC_LEN=$(wc -c < "$ENC" 2>/dev/null || echo 0)
    record "AES-256-CBC encrypt data" "$(check_rc $RC_ENC)" \
        "plaintext=${PLAIN_LEN}B ciphertext=${ENC_LEN}B"
else
    record "generate AES-256 key + IV (48 B)" FAIL "PEM not available"
    record "RSA-wrap AES key with public key" FAIL "PEM not available"
    record "AES-256-CBC encrypt data" FAIL "PEM not available"
fi

# ---------------------------------------------------------------------------
# FT25.4 — Hybrid decrypt: PKCS#11 unwraps AES key, AES decrypts data
# ---------------------------------------------------------------------------
section "$FT.4: Hybrid decrypt (PKCS#11 unwraps AES key, AES-CBC decrypts data)"

RC_DEC=1
if [ "$RC_ENC" -eq 0 ]; then
    # Unwrap AES key with TEE RSA private key
    pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
        --decrypt --mechanism RSA-PKCS --id 25 --type privkey \
        --input-file "$KEYMATERIAL_ENC" --output-file "$KEYMATERIAL_DEC" 2>&1
    record "PKCS#11 RSA-unwrap AES key" "$(check_rc $?)"

    # Extract recovered key and IV
    AES_KEY_DEC=$(od -A n -t x1 "$KEYMATERIAL_DEC" | tr -d ' \n' | cut -c1-64)
    AES_IV_DEC=$(od  -A n -t x1 "$KEYMATERIAL_DEC" | tr -d ' \n' | cut -c65-96)

    # Decrypt data with recovered AES key
    openssl enc -d -aes-256-cbc -K "$AES_KEY_DEC" -iv "$AES_IV_DEC" \
        -nosalt -in "$ENC" -out "$DEC" 2>&1
    RC_DEC=$?
    record "AES-256-CBC decrypt data" "$(check_rc $RC_DEC)"
else
    record "PKCS#11 RSA-unwrap AES key" FAIL "encryption failed, skipping"
    record "AES-256-CBC decrypt data"   FAIL "encryption failed, skipping"
fi

# ---------------------------------------------------------------------------
# FT25.5 — Roundtrip: decrypted == plaintext
# ---------------------------------------------------------------------------
section "$FT.5: Roundtrip integrity check"

if [ "$RC_DEC" -eq 0 ]; then
    DEC_LEN=$(wc -c < "$DEC" 2>/dev/null || echo 0)
    IN_MD5=$(md5sum "$PLAIN" | awk '{print $1}')
    OUT_MD5=$(md5sum "$DEC"  | awk '{print $1}')
    echo "  input  : ${PLAIN_LEN}B  md5=$IN_MD5"
    echo "  output : ${DEC_LEN}B  md5=$OUT_MD5"

    if cmp -s "$PLAIN" "$DEC"; then
        record "decrypted == plaintext" PASS \
            "${PLAIN_LEN} bytes match exactly"
    else
        record "decrypted == plaintext" FAIL \
            "mismatch: plain=${PLAIN_LEN}B dec=${DEC_LEN}B"
    fi
else
    record "decrypted == plaintext" FAIL "decrypt failed, skipping"
fi

# ---------------------------------------------------------------------------
# FT25.6 — Private key non-extractable
# ---------------------------------------------------------------------------
section "$FT.6: Private key non-extractable (CKA_SENSITIVE)"

# pkcs11-tool --read-object --type privkey returns RC=0 with the message
# "sorry, reading private keys not (yet) supported" but writes NO file.
# The authoritative check is therefore: no key material written to disk.
rm -f /tmp/ft25_priv_attempt.bin
pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --read-object --type privkey --id 25 --output-file /tmp/ft25_priv_attempt.bin \
    2>&1
RC_EXTRACT=$?
if [ ! -s /tmp/ft25_priv_attempt.bin ]; then
    record "privkey non-extractable (no key material written)" PASS \
        "extraction refused (RC=$RC_EXTRACT, output file empty/not created)"
else
    record "privkey non-extractable (no key material written)" FAIL \
        "private key value was exported — CKA_SENSITIVE not enforced!"
fi
rm -f /tmp/ft25_priv_attempt.bin

# Verify Access field in token shows "never extractable"
PRIV_ACCESS=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN1" --login --pin "$UPIN" \
    --list-objects --type privkey --id 25 2>&1)
echo "$PRIV_ACCESS" | grep -qi "never extractable"
record "CKA_EXTRACTABLE=FALSE (Access: never extractable)" "$(check_rc $?)"

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
del_obj "$TOKEN1" privkey ft25-rsa 2>/dev/null
del_obj "$TOKEN1" pubkey  ft25-rsa 2>/dev/null
rm -f "$PLAIN" "$ENC" "$DEC" "$PUB_DER" "$PUB_PEM" \
      "$KEYMATERIAL" "$KEYMATERIAL_ENC" "$KEYMATERIAL_DEC"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
section "$FT: Summary ($OK/$TOTAL passed)"
echo
printf '%b' "$RESULTS" | while IFS='=' read -r name result; do
    [ -z "$name" ] && continue
    printf "  %-42s : %s\n" "$name" "$result"
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
