#!/bin/sh
# ft19-target.sh — RSA TEE keypair: encrypt with public key, decrypt with TEE private key
# Run directly on the Sparrow-Hawk board.
#
# Workflow:
#   1. Create plaintext file
#   2. Ensure PKCS#11 token is initialised
#   3. Generate RSA-2048 keypair inside OP-TEE (private key never leaves TEE)
#   4. Export public key as DER → PEM via openssl
#   5. Encrypt file with public key (openssl, userspace)
#   6. Decrypt ciphertext with TEE private key (pkcs11-tool C_Decrypt)
#   7. Compare decrypted output to original plaintext
#   8. Show w77q-dump flash LUT to confirm key is persisted
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  RSA TEE keypair test: encrypt with public key, decrypt with TEE private key."
    echo "  Private key is generated inside OP-TEE and never leaves the TEE."
    echo ""
    echo "Pass/Fail criteria: PASS if decrypted output matches original plaintext."
    exit 0 ;;
esac

set -e

MOD=/usr/lib/libckteec.so
TOKEN=optee-test-token
SOPIN=12345678
UPIN=87654321
LABEL=rsa-tee-enc
ID=19

PLAIN=/tmp/ft19_plain.bin
PUBDER=/tmp/ft19_pub.der
PUBPEM=/tmp/ft19_pub.pem
CIPHER=/tmp/ft19_cipher.bin
DECRYPTED=/tmp/ft19_decrypted.bin

PASS=0

die() { echo "FAIL: $*" >&2; exit 1; }

echo "════════════════════════════════════════"
echo "  FT19 — RSA TEE keygen + encrypt/decrypt"
echo "════════════════════════════════════════"

# ── STEP 1: create random plaintext (50 – 220 bytes) ─────────────────────────
echo
echo "── STEP 1: create random plaintext file (50–220 B, TEE C_GenerateRandom) ──"
RAND_EXTRA=$(( $(pkcs11-tool --module "$MOD" --generate-random 1 | od -A n -N1 -t u1 | tr -d ' ') % 171 ))
RAND_LEN=$(( 50 + RAND_EXTRA ))
pkcs11-tool --module "$MOD" --generate-random "$RAND_LEN" > "$PLAIN" 2>/dev/null
PLAIN_LEN=$(wc -c < "$PLAIN")
echo "  random plaintext: $PLAIN_LEN bytes"

# ── STEP 2: ensure token initialised ─────────────────────────────────────────
echo
echo "── STEP 2: ensure PKCS#11 token ready ──"
if ! pkcs11-tool --module "$MOD" -T 2>&1 | grep -q "token label.*:.*$TOKEN"; then
    echo "  initialising token…"
    pkcs11-tool --module "$MOD" --slot-index 0 \
        --init-token --label "$TOKEN" --so-pin "$SOPIN"
    pkcs11-tool --module "$MOD" --token-label "$TOKEN" \
        --login --login-type so --so-pin "$SOPIN" \
        --init-pin --new-pin "$UPIN"
    echo "  token initialised"
else
    echo "  token OK"
fi

# Remove any stale key with the same ID
pkcs11-tool --module "$MOD" --token-label "$TOKEN" --login --pin "$UPIN" \
    --delete-object --type privkey --id "$ID" 2>/dev/null || true
pkcs11-tool --module "$MOD" --token-label "$TOKEN" --login --pin "$UPIN" \
    --delete-object --type pubkey  --id "$ID" 2>/dev/null || true

# ── STEP 3: generate RSA-2048 keypair in TEE ─────────────────────────────────
echo
echo "── STEP 3: generate RSA-2048 keypair in TEE ──"
pkcs11-tool --module "$MOD" --token-label "$TOKEN" \
    --login --pin "$UPIN" \
    --keypairgen --key-type rsa:2048 \
    --label "$LABEL" --id "$ID" \
    --usage-decrypt --usage-sign \
    || die "RSA key pair generation failed"
echo "  ✓ RSA-2048 key pair generated in TEE"

# ── STEP 4: export public key ─────────────────────────────────────────────────
echo
echo "── STEP 4: export public key from TEE ──"
pkcs11-tool --module "$MOD" --token-label "$TOKEN" \
    --read-object --type pubkey --id "$ID" \
    --output-file "$PUBDER" \
    || die "public key export (DER) failed"

openssl rsa -pubin -inform DER -in "$PUBDER" -outform PEM -out "$PUBPEM" \
    || die "DER→PEM conversion failed"

echo "  ✓ public key: $(wc -c < "$PUBPEM") bytes PEM"

# ── STEP 5: encrypt with public key directly (fits in RSA-PKCS1 max 245 B) ───
echo
echo "── STEP 5: encrypt with public key (openssl pkeyutl, RSA-PKCS1) ──"
openssl pkeyutl -encrypt -pubin -inkey "$PUBPEM" \
    -pkeyopt rsa_padding_mode:pkcs1 \
    -in "$PLAIN" -out "$CIPHER" \
    || die "openssl encrypt failed"
echo "  ✓ ciphertext: $(wc -c < "$CIPHER") bytes"

# ── STEP 6: decrypt with TEE private key (PKCS#11) ────────────────────────────
echo
echo "── STEP 6: decrypt with TEE private key (PKCS#11 C_Decrypt) ──"
pkcs11-tool --module "$MOD" --token-label "$TOKEN" \
    --login --pin "$UPIN" \
    --decrypt --mechanism RSA-PKCS --id "$ID" \
    --input-file "$CIPHER" --output-file "$DECRYPTED" \
    || die "PKCS#11 decrypt failed"
echo "  ✓ decrypted: $(wc -c < "$DECRYPTED") bytes"

# ── STEP 7: compare input vs output ──────────────────────────────────────────
echo
echo "── STEP 7: compare input vs decrypted output ──"

IN_SIZE=$(wc -c < "$PLAIN")
OUT_SIZE=$(wc -c < "$DECRYPTED")
IN_MD5=$(md5sum "$PLAIN"      | awk '{print $1}')
OUT_MD5=$(md5sum "$DECRYPTED" | awk '{print $1}')

echo "  input    : $IN_SIZE bytes  md5=$IN_MD5"
echo "  output   : $OUT_SIZE bytes  md5=$OUT_MD5"

if cmp -s "$PLAIN" "$DECRYPTED"; then
    echo "  ✓ byte-for-byte match (cmp OK, md5 match)"
    PASS=1
else
    echo "  ✗ mismatch"
    PASS=0
fi

# ── STEP 8: w77q-dump flash LUT ───────────────────────────────────────────────
echo
echo "── STEP 8: w77q-dump list-all ──"
w77q-dump list-all 2>&1 || true

# ── SUMMARY ───────────────────────────────────────────────────────────────────
echo
echo "════════════════════════════════════════"
echo "  SUMMARY"
echo "════════════════════════════════════════"
echo "  RSA-2048 keypair in TEE    : ✓"
echo "  Public key exported        : ✓"
echo "  Encrypt (RSA-PKCS1 direct) : ✓"
echo "  Plaintext size             : $IN_SIZE bytes (random 50-220 B)"
if [ "$PASS" -eq 1 ]; then
    echo "  Decrypt (PKCS#11+AES)      : ✓"
    echo "  Plaintext match            : PASS"
    exit 0
else
    echo "  Decrypt (PKCS#11+AES)      : ✗"
    echo "  Plaintext match            : FAIL"
    exit 1
fi
