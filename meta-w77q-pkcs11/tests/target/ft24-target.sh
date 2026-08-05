#!/bin/sh
# ft24-target.sh — TEE Isolation and Data Protection Validation
#
# FT24.1 — Token namespace isolation: TOKEN1 objects invisible to TOKEN0
# FT24.2 — Private object access control: requires --login to see
# FT24.3 — Wrong PIN rejection: bad PIN returns error
# FT24.4 — Data integrity roundtrip: write/read back/compare byte-for-byte
# FT24.5 — Encryption at rest: plaintext not found verbatim in raw flash
# FT24.6 — TA UUID tagging: flash record carries PKCS#11 TA UUID
#
# PKCS#11 TA UUID (fd02c9da-306c-48c7-{a4,9c,...}) stored on LE flash as:
#   dac902fd-6c30-c748-a49c-bbd827ae86ee
#
# Deploy to /usr/bin/ft24-target.sh
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  TEE isolation and data protection:"
    echo "    FT24.1  Token namespace isolation (cross-token object visibility)"
    echo "    FT24.2  Private object access control (login required)"
    echo "    FT24.3  Wrong PIN rejection"
    echo "    FT24.4  Data integrity roundtrip (write / read-back / byte compare)"
    echo "    FT24.5  Encryption at rest (plaintext not verbatim in flash)"
    echo "    FT24.6  TA UUID tagging in flash record"
    echo ""
    echo "Pass/Fail criteria: PASS if all isolation and protection checks succeed."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

FT=FT24
PASS=1
TOTAL=0
OK=0
RESULTS=""

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

# PKCS#11 TA UUID as printed by w77q-dump (raw LE bytes of the TEE_UUID struct)
PKCS11_TA_UUID_FLASH="dac902fd-6c30-c748-a49c-bbd827ae86ee"

# Extract flat lowercase hex from w77q-dump read-raw output
flat_hex() {
    awk '/^\[OK\]/ { next }
         NF > 1 {
             for (f = 2; f <= NF; f++) {
                 v = $f
                 if (v ~ /^\|/) break
                 if (v ~ /^[0-9a-fA-F][0-9a-fA-F]$/) printf "%s", tolower(v)
             }
         }'
}

section "$FT: TEE Isolation and Data Protection"
ensure_tee_supplicant
ensure_token "$TOKEN0" 0
ensure_token "$TOKEN1" 1

# Stale object cleanup
del_obj "$TOKEN1" data ft24-iso     2>/dev/null
del_obj "$TOKEN1" data ft24-priv    2>/dev/null
del_obj "$TOKEN1" data ft24-data    2>/dev/null

# ---------------------------------------------------------------------------
# FT24.1 — Token namespace isolation
# ---------------------------------------------------------------------------
section "$FT.1: Token namespace isolation"

printf 'ft24-isolation-payload' | \
    pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
        --login --pin "$UPIN" \
        --write-object /dev/stdin --type data --label ft24-iso 2>&1
record "write ft24-iso to TOKEN1" "$(check_rc $?)"

# Object MUST appear in TOKEN1 listing
T1_LIST=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
              --login --pin "$UPIN" -O 2>&1)
echo "$T1_LIST" | grep -q "ft24-iso"
record "ft24-iso visible in TOKEN1" "$(check_rc $?)"

# Object must NOT appear in TOKEN0 listing
T0_LIST=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN0" \
              --login --pin "$UPIN" -O 2>&1)
echo "$T0_LIST" | grep -q "ft24-iso"
if [ $? -eq 0 ]; then
    record "ft24-iso absent from TOKEN0" FAIL "object leaked to TOKEN0"
else
    record "ft24-iso absent from TOKEN0" PASS
fi

del_obj "$TOKEN1" data ft24-iso 2>/dev/null

# ---------------------------------------------------------------------------
# FT24.2 — Private object access control
# ---------------------------------------------------------------------------
section "$FT.2: Private object access control"

printf 'ft24-private-payload' | \
    pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
        --login --pin "$UPIN" \
        --write-object /dev/stdin --type data --label ft24-priv \
        --private 2>&1
record "write private ft24-priv to TOKEN1" "$(check_rc $?)"

# Without login: CKA_PRIVATE=TRUE objects must NOT appear
NOLOGIN=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN1" -O 2>&1)
echo "$NOLOGIN" | grep -q "ft24-priv"
if [ $? -eq 0 ]; then
    record "ft24-priv hidden without login" FAIL "private object visible without PIN"
else
    record "ft24-priv hidden without login" PASS
fi

# With login: object MUST appear
LOGIN=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
            --login --pin "$UPIN" -O 2>&1)
echo "$LOGIN" | grep -q "ft24-priv"
record "ft24-priv visible with login" "$(check_rc $?)"

del_obj "$TOKEN1" data ft24-priv 2>/dev/null

# ---------------------------------------------------------------------------
# FT24.3 — Wrong PIN rejection
# ---------------------------------------------------------------------------
section "$FT.3: Wrong PIN rejection"

WRONG_OUT=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
                --login --pin "00000000" -O 2>&1)
RC=$?
echo "  pkcs11-tool with wrong PIN RC=$RC"
echo "  output: $WRONG_OUT" | head -3
if [ "$RC" -ne 0 ]; then
    record "wrong PIN rejected (RC!=0)" PASS
else
    record "wrong PIN rejected (RC!=0)" FAIL "wrong PIN was accepted!"
fi

# ---------------------------------------------------------------------------
# FT24.4 + FT24.5 — Data integrity roundtrip + encryption at rest
# (combined: LUT snapshot around the write captures the exact flash entry)
# ---------------------------------------------------------------------------
section "$FT.4: Data integrity roundtrip + FT24.5: Encryption at rest"

PAYLOAD="FT24DataIntegrityTest32BytesX1234"   # exactly 32 bytes
PLAIN_HEX=$(printf '%s' "$PAYLOAD" | od -A n -t x1 | tr -d ' \n')
PLAIN_LEN=32

LUT_BEFORE=/tmp/ft24_lut_before.txt
LUT_AFTER=/tmp/ft24_lut_after.txt

# Snapshot BEFORE
w77q-dump list-all > "$LUT_BEFORE" 2>&1

# Write ft24-data
printf '%s' "$PAYLOAD" | \
    pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
        --login --pin "$UPIN" \
        --write-object /dev/stdin --type data --label ft24-data 2>&1
RC_WRITE=$?
record "write ft24-data to TOKEN1" "$(check_rc $RC_WRITE)"

if [ "$RC_WRITE" -ne 0 ]; then
    record "read back matches written data" FAIL "write failed, skipping"
    record "data_size > plaintext size" FAIL "write failed, skipping"
    record "plaintext not in raw flash" FAIL "write failed, skipping"
    record "TA UUID in flash is PKCS#11 UUID" FAIL "write failed, skipping"
else
    # Snapshot AFTER
    w77q-dump list-all > "$LUT_AFTER" 2>&1

    # FT24.4 — read back and compare
    READ_BIN=/tmp/ft24_read.bin
    pkcs11-tool --module "$MOD" --token-label "$TOKEN1" \
        --login --pin "$UPIN" \
        --read-object --type data --label ft24-data \
        --output-file "$READ_BIN" 2>&1
    RC_READ=$?

    if [ "$RC_READ" -eq 0 ]; then
        READ_HEX=$(od -A n -t x1 < "$READ_BIN" | tr -d ' \n')
        if [ "$READ_HEX" = "$PLAIN_HEX" ]; then
            record "read back matches written data" PASS
        else
            record "read back matches written data" FAIL \
                "expected $PLAIN_HEX got $READ_HEX"
        fi
    else
        record "read back matches written data" FAIL "read-object failed RC=$RC_READ"
    fi

    # FT24.5 + FT24.6 — find new flash entry, check encryption + UUID
    # New entry: in AFTER but not in BEFORE (strip index field for comparison)
    awk '{$1=""; print}' "$LUT_BEFORE" > /tmp/ft24_before_strip.txt
    awk '{$1=""; print}' "$LUT_AFTER"  > /tmp/ft24_after_strip.txt
    NEW_LINE=$(grep -Fxvf /tmp/ft24_before_strip.txt /tmp/ft24_after_strip.txt \
               | grep 'flash_off' | head -1)

    if [ -z "$NEW_LINE" ]; then
        echo "  WARNING: no new LUT entry found after write"
        record "data_size > plaintext size" FAIL "flash entry not found"
        record "plaintext not in raw flash" FAIL "flash entry not found"
        record "TA UUID in flash is PKCS#11 UUID" FAIL "flash entry not found"
    else
        echo "  new LUT entry:$NEW_LINE"

        FLASH_OFF=$(echo "$NEW_LINE" | grep -oE 'flash_off=0x[0-9a-fA-F]+' \
                    | grep -oE '0x[0-9a-fA-F]+')
        DATA_SIZE=$(echo "$NEW_LINE" | grep -oE 'data_size=[[:space:]]*[0-9]+' \
                    | grep -oE '[0-9]+$')
        ENTRY_UUID=$(echo "$NEW_LINE" | grep -oE 'ta_uuid=[0-9a-f-]+' | cut -d= -f2)

        # FT24.5a — data_size > plaintext (OP-TEE adds encryption overhead)
        if [ -n "$DATA_SIZE" ] && [ "$DATA_SIZE" -gt "$PLAIN_LEN" ]; then
            record "data_size > plaintext size" PASS \
                "flash=${DATA_SIZE}B > plaintext=${PLAIN_LEN}B"
        else
            record "data_size > plaintext size" FAIL \
                "flash=${DATA_SIZE}B plaintext=${PLAIN_LEN}B"
        fi

        # FT24.5b — plaintext hex NOT verbatim in raw flash data
        if [ -n "$FLASH_OFF" ] && [ -n "$DATA_SIZE" ] && [ "$DATA_SIZE" -gt 0 ]; then
            READ_SZ=$DATA_SIZE
            [ "$READ_SZ" -gt 65000 ] && READ_SZ=65000
            RAW_HEX=$(w77q-dump read-raw "$FLASH_OFF" "$((104 + READ_SZ))" 2>/dev/null \
                      | flat_hex)
            if echo "$RAW_HEX" | grep -qF "$PLAIN_HEX"; then
                record "plaintext not in raw flash" FAIL \
                    "payload found verbatim at $FLASH_OFF (storage not encrypted!)"
            else
                record "plaintext not in raw flash" PASS \
                    "payload absent from raw flash bytes"
            fi
        else
            record "plaintext not in raw flash" FAIL "could not read flash entry"
        fi

        # FT24.6 — ta_uuid matches PKCS#11 TA UUID
        if [ "$ENTRY_UUID" = "$PKCS11_TA_UUID_FLASH" ]; then
            record "TA UUID in flash is PKCS#11 UUID" PASS "$ENTRY_UUID"
        else
            record "TA UUID in flash is PKCS#11 UUID" FAIL \
                "got $ENTRY_UUID expected $PKCS11_TA_UUID_FLASH"
        fi
    fi

    rm -f /tmp/ft24_before_strip.txt /tmp/ft24_after_strip.txt
fi

# Cleanup
del_obj "$TOKEN1" data ft24-data 2>/dev/null
rm -f "$LUT_BEFORE" "$LUT_AFTER" "$READ_BIN"

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
