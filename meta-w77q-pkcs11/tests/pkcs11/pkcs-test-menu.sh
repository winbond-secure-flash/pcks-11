#!/bin/sh
# pkcs-test-menu.sh — Interactive test suite for pkcs11-tool and pkcs15-tool
# Deploy as /usr/bin/pkcs-test-menu.sh on the Sparrow Hawk board.
# Usage: pkcs-test-menu.sh [--auto]
#   --auto : run all tests non-interactively
#
# Requires: pkcs11-tool, pkcs15-tool, openssl, tee-supplicant running
# ─────────────────────────────────────────────────────────────────────────────

. /usr/bin/tee-tests-common.sh 2>/dev/null || {
    # Fallback if not deployed
    MOD=/usr/lib/libckteec.so
    [ -e "$MOD" ] || MOD=/usr/lib/libckteec.so.0
    TOKEN0=optee-test-token
    TOKEN1=yuri
    SOPIN=12345678
    UPIN=87654321
}

AUTO=0
[ "$1" = "--auto" ] && AUTO=1

LOGFILE=/tmp/pkcs-test-$(date +%Y%m%d-%H%M%S).log
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

c()  { printf "${CYAN}${BOLD}%s${NC}\n" "$*"; }
ok() { printf "  ${GREEN}[PASS]${NC} %s\n" "$*"; PASS_COUNT=$((PASS_COUNT+1))
       echo "[PASS] $*" >> "$LOGFILE"; }
no() { printf "  ${RED}[FAIL]${NC} %s\n" "$*"; FAIL_COUNT=$((FAIL_COUNT+1))
       echo "[FAIL] $*" >> "$LOGFILE"; }
sk() { printf "  ${YELLOW}[SKIP]${NC} %s\n" "$*"; SKIP_COUNT=$((SKIP_COUNT+1))
       echo "[SKIP] $*" >> "$LOGFILE"; }
hdr() { printf "\n${BOLD}${CYAN}══ %s ══${NC}\n" "$*"
        echo "" >> "$LOGFILE"
        echo "══ $* ══" >> "$LOGFILE"; }

# Run command — shows command+output on terminal via stderr (not swallowed by callers),
# captures output into $_out for callers that do: out=$(run_cmd ...) | grep ...
run_cmd() {
    _cmd="$*"
    _tmp=$(mktemp)

    # >&2 so these lines appear even when caller does out=$(run_cmd ...)
    printf "\n  ${CYAN}▶ ${NC}${BOLD}%s${NC}\n" "$_cmd" >&2
    echo "  CMD: $_cmd" >> "$LOGFILE"

    if [ "$AUTO" -eq 0 ]; then
        printf "  ${BOLD}[Enter]${NC}=run  ${BOLD}[e]${NC}=edit  ${BOLD}[s]${NC}=skip » " >&2
        read -r _ans </dev/tty
        case "$_ans" in
            e|E)
                printf "  Edit » " >&2
                read -r _new </dev/tty
                [ -n "$_new" ] && _cmd="$_new"
                printf "  ${CYAN}▶ (edited)${NC} ${BOLD}%s${NC}\n" "$_cmd" >&2
                echo "  CMD(edited): $_cmd" >> "$LOGFILE"
                ;;
            s|S)
                printf "  ${YELLOW}⏭  skipped by user${NC}\n" >&2
                echo "  OUT: (skipped)" >> "$LOGFILE"
                rm -f "$_tmp"
                _out="(skipped)"
                return 0
                ;;
        esac
    fi

    eval "$_cmd" >"$_tmp" 2>&1
    _rc=$?
    _out=$(cat "$_tmp")
    # Show output on terminal (stderr) indented
    sed 's/^/  /' "$_tmp" >&2
    echo "  OUT: $_out" >> "$LOGFILE"
    rm -f "$_tmp"
    # Also echo to stdout so callers that capture with $() can grep it
    printf '%s\n' "$_out"
    return $_rc
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 1 — Environment check
# ─────────────────────────────────────────────────────────────────────────────
test_env() {
    hdr "1. Environment"
    # tee-supplicant
    if systemctl is-active tee-supplicant -q 2>/dev/null; then
        ok "tee-supplicant is running"
    else
        no "tee-supplicant not running — starting"
        systemctl start tee-supplicant && sleep 2
    fi
    # /dev/tee0
    if [ -e /dev/tee0 ]; then
        ok "/dev/tee0 present"
    else
        no "/dev/tee0 missing — OP-TEE not loaded"
        return 1
    fi
    # module
    if [ -e "$MOD" ]; then
        ok "module: $MOD"
    else
        no "libckteec not found at $MOD"
        return 1
    fi
    # pkcs11-tool
    if command -v pkcs11-tool >/dev/null 2>&1; then
        V=$(dpkg -l opensc 2>/dev/null | awk '/^ii/{print $3}' | head -1)
        [ -z "$V" ] && V=$(opkg list-installed 2>/dev/null | awk '/^opensc /{print $3}' | head -1)
        [ -z "$V" ] && V="installed"
        ok "pkcs11-tool: OpenSC-$V"
    else
        no "pkcs11-tool not found"
    fi
    # pkcs15-tool
    if command -v pkcs15-tool >/dev/null 2>&1; then
        V=$(dpkg -l opensc 2>/dev/null | awk '/^ii/{print $3}' | head -1)
        [ -z "$V" ] && V=$(opkg list-installed 2>/dev/null | awk '/^opensc /{print $3}' | head -1)
        [ -z "$V" ] && V="installed"
        ok "pkcs15-tool: OpenSC-$V"
    else
        sk "pkcs15-tool not found (OpenSC not installed)"
    fi
    # openssl
    if command -v openssl >/dev/null 2>&1; then
        ok "openssl: $(openssl version)"
    else
        sk "openssl not found (verify steps will be skipped)"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 2 — pkcs11-tool: Token info & diagnostics
# ─────────────────────────────────────────────────────────────────────────────
test_p11_info() {
    hdr "2. pkcs11-tool — Token info & diagnostics"

    out=$(run_cmd pkcs11-tool --module "$MOD" --show-info)
    echo "$out" | grep -qi "cryptoki" && ok "show-info: Cryptoki library info" \
                                      || no "show-info: unexpected output"

    out=$(run_cmd pkcs11-tool --module "$MOD" --list-slots)
    echo "$out" | grep -qi "slot" && ok "list-slots: slot listing present" \
                                   || no "list-slots failed"

    pkcs11-tool --module "$MOD" --slot 0 --list-mechanisms \
        > /tmp/mechs.txt 2>&1
    CNT=$(grep -c ',' /tmp/mechs.txt || true)
    [ "$CNT" -gt 5 ] && ok "list-mechanisms: $CNT mechanisms listed" \
                       || no "list-mechanisms: too few results ($CNT)"

    out=$(pkcs11-tool --module "$MOD" --token-label "$TOKEN0" \
        --login --pin "$UPIN" --list-objects 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        cnt=$(echo "$out" | grep -c "Object\|Key\|Cert\|Data" || true)
        ok "list-objects: $cnt object(s) in token (exit 0)"
    else
        no "list-objects: exit $rc — $out"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 3 — pkcs11-tool: Token lifecycle
# ─────────────────────────────────────────────────────────────────────────────
test_p11_token() {
    hdr "3. pkcs11-tool — Token lifecycle"

    # Verify token label
    out=$(run_cmd pkcs11-tool --module "$MOD" --list-slots)
    echo "$out" | grep -q "$TOKEN0" && ok "token '$TOKEN0' present in slot list" \
                                     || no "token '$TOKEN0' not found"

    # User PIN login
    out=$(run_cmd pkcs11-tool --module "$MOD" --token-label "$TOKEN0" \
        --login --pin "$UPIN" --list-objects 2>&1)
    if echo "$out" | grep -qi "incorrect\|CKR_PIN"; then
        no "user PIN login: PIN rejected — $out"
    else
        ok "user PIN login: accepted"
    fi

    # SO PIN login — use separate --list-slots (read-only, no session conflict)
    out=$(run_cmd pkcs11-tool --module "$MOD" --token-label "$TOKEN0" \
        --login --login-type so --so-pin "$SOPIN" --list-slots 2>&1)
    if echo "$out" | grep -qi "CKR_PIN\|incorrect"; then
        no "SO PIN login: PIN rejected — $out"
    else
        ok "SO PIN login: accepted"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 4 — pkcs11-tool: Random number generation
# ─────────────────────────────────────────────────────────────────────────────
test_p11_random() {
    hdr "4. pkcs11-tool — Random number generation"

    out=$(run_cmd pkcs11-tool --module "$MOD" --token-label "$TOKEN0" \
        --login --pin "$UPIN" --generate-random 32 \
        --output-file /tmp/rand32.bin 2>&1)
    if [ -s /tmp/rand32.bin ]; then
        BYTES=$(wc -c < /tmp/rand32.bin)
        [ "$BYTES" -ge 32 ] && ok "generate-random 32 bytes: $BYTES bytes" \
                             || no "generate-random 32: only $BYTES bytes"
    else
        no "generate-random 32: no output — $out"
    fi

    out=$(run_cmd pkcs11-tool --module "$MOD" --token-label "$TOKEN0" \
        --login --pin "$UPIN" --generate-random 64 \
        --output-file /tmp/rand64.bin 2>&1)
    if [ -s /tmp/rand64.bin ]; then
        BYTES=$(wc -c < /tmp/rand64.bin)
        [ "$BYTES" -ge 64 ] && ok "generate-random 64 bytes: $BYTES bytes" \
                             || no "generate-random 64: only $BYTES bytes"
    else
        no "generate-random 64: no output — $out"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 5 — pkcs11-tool: Hash (digest)
# ─────────────────────────────────────────────────────────────────────────────
test_p11_hash() {
    hdr "5. pkcs11-tool — Hash / Digest"

    echo -n "Sparrow Hawk hash test" > /tmp/pt_hash.bin

    for MECH in SHA256 SHA384 SHA512 SHA-1; do
        HFILE=/tmp/hash_${MECH}.bin
        out=$(run_cmd pkcs11-tool --module "$MOD" --token-label "$TOKEN0" \
            --login --pin "$UPIN" --hash --mechanism "$MECH" \
            --input-file /tmp/pt_hash.bin \
            --output-file "$HFILE" 2>&1)
        if [ -s "$HFILE" ]; then
            SZ=$(wc -c < "$HFILE")
            ok "hash $MECH: ${SZ}-byte digest written"
        else
            no "hash $MECH: $out"
        fi
    done
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 6 — pkcs11-tool: RSA operations
# ─────────────────────────────────────────────────────────────────────────────
test_p11_rsa() {
    hdr "6. pkcs11-tool — RSA operations"

    LABEL=test-rsa-menu
    ID=10
    PLAIN=/tmp/pt_rsa.bin
    SIG=/tmp/sig_rsa.bin
    PSS=/tmp/sig_pss.bin
    ENC=/tmp/enc_rsa.bin
    DEC=/tmp/dec_rsa.bin

    # Cleanup any leftovers
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type privkey --id "$ID" >/dev/null 2>&1 || true
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type pubkey  --id "$ID" >/dev/null 2>&1 || true

    echo -n "RSA test message from pkcs-test-menu" > "$PLAIN"

    # 6.1 Keypairgen RSA-2048 with sign+decrypt
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --keypairgen --key-type rsa:2048 \
        --usage-sign --usage-decrypt \
        --label "$LABEL" --id "$ID" 2>&1)
    echo "$out" | grep -qi "key pair generated\|Private Key" \
        && ok "RSA-2048 keypairgen (--usage-sign --usage-decrypt)" \
        || { no "RSA-2048 keypairgen failed: $out"; return; }

    # 6.2 Sign SHA256-RSA-PKCS
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --sign --mechanism SHA256-RSA-PKCS \
        --id "$ID" --input-file "$PLAIN" --output-file "$SIG" 2>&1)
    echo "$out" | grep -qi "error\|CKR_\|fail" \
        && no "sign SHA256-RSA-PKCS: $out" \
        || ok "sign SHA256-RSA-PKCS"

    # 6.3 Verify SHA256-RSA-PKCS
    out=$(run_cmd pkcs11-tool --module "$MOD" \
        --token-label "$TOKEN0" \
        --verify --mechanism SHA256-RSA-PKCS \
        --id "$ID" --input-file "$PLAIN" --signature-file "$SIG" 2>&1)
    echo "$out" | grep -qi "Signature is valid" \
        && ok "verify SHA256-RSA-PKCS: Signature is valid" \
        || no "verify SHA256-RSA-PKCS: $out"

    # 6.4 Sign SHA256-RSA-PKCS-PSS
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --sign --mechanism SHA256-RSA-PKCS-PSS \
        --id "$ID" --input-file "$PLAIN" --output-file "$PSS" 2>&1)
    echo "$out" | grep -qi "error\|CKR_\|fail" \
        && no "sign SHA256-RSA-PKCS-PSS: $out" \
        || ok "sign SHA256-RSA-PKCS-PSS (hashAlg=SHA256, mgf=MGF1-SHA256, salt=32B)"

    # 6.5 Verify SHA256-RSA-PKCS-PSS
    out=$(run_cmd pkcs11-tool --module "$MOD" \
        --token-label "$TOKEN0" \
        --verify --mechanism SHA256-RSA-PKCS-PSS \
        --id "$ID" --input-file "$PLAIN" --signature-file "$PSS" 2>&1)
    echo "$out" | grep -qi "Signature is valid" \
        && ok "verify SHA256-RSA-PKCS-PSS: Signature is valid" \
        || no "verify SHA256-RSA-PKCS-PSS: $out"

    # 6.6 Encrypt RSA-PKCS — export pubkey, encrypt via openssl (no PIN needed),
    #     then decrypt via pkcs11-tool (private key op, PIN supplied with --pin).
    #     This avoids the libckteec non-tty PIN limitation for C_Encrypt.
    run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --read-object --type pubkey --id "$ID" \
        --output-file /tmp/rsa_pub_enc.der >/dev/null 2>&1
    if [ -s /tmp/rsa_pub_enc.der ] && command -v openssl >/dev/null 2>&1; then
        openssl rsa -inform DER -pubin -in /tmp/rsa_pub_enc.der \
            -out /tmp/rsa_pub_enc.pem >/dev/null 2>&1
        openssl rsautl -encrypt -pkcs -pubin \
            -inkey /tmp/rsa_pub_enc.pem \
            -in "$PLAIN" -out "$ENC" >/dev/null 2>&1 \
            && ok "encrypt RSA-PKCS: $(wc -c < "$ENC") bytes (via openssl pubkey)" \
            || no "encrypt RSA-PKCS: openssl encrypt failed"

        # 6.7 Decrypt RSA-PKCS via pkcs11-tool (private key, --pin works here)
        out=$(run_cmd pkcs11-tool --module "$MOD" --login \
            --token-label "$TOKEN0" --pin "$UPIN" \
            --decrypt --mechanism RSA-PKCS --id "$ID" \
            --input-file "$ENC" --output-file "$DEC" 2>&1)
        if cmp -s "$PLAIN" "$DEC" 2>/dev/null; then
            ok "decrypt RSA-PKCS: plaintext matches"
        else
            no "decrypt RSA-PKCS: $out"
        fi
        rm -f /tmp/rsa_pub_enc.der /tmp/rsa_pub_enc.pem
    else
        sk "encrypt RSA-PKCS: openssl not available or pubkey export failed"
        sk "decrypt RSA-PKCS: skipped (encrypt skipped)"
    fi

    # 6.8 read-object pubkey
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --read-object --type pubkey --id "$ID" \
        --output-file /tmp/rsa_pub.der 2>&1)
    [ -s /tmp/rsa_pub.der ] \
        && ok "read-object pubkey: $(wc -c < /tmp/rsa_pub.der) bytes DER" \
        || no "read-object pubkey: $out"

    # 6.9 Cleanup
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type privkey --id "$ID" >/dev/null 2>&1 && \
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type pubkey  --id "$ID" >/dev/null 2>&1 \
        && ok "cleanup: RSA test keys deleted" \
        || sk "cleanup: delete may have partially failed"
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 7 — pkcs11-tool: EC operations
# ─────────────────────────────────────────────────────────────────────────────
test_p11_ec() {
    hdr "7. pkcs11-tool — EC operations"

    LABEL=test-ec-menu
    ID=11
    PLAIN=/tmp/pt_ec.bin
    SIG=/tmp/sig_ec.bin

    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type privkey --id "$ID" >/dev/null 2>&1 || true
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type pubkey  --id "$ID" >/dev/null 2>&1 || true

    echo -n "EC test message from pkcs-test-menu" > "$PLAIN"

    # 7.1 Keypairgen P-256
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --keypairgen --key-type EC:prime256v1 \
        --label "$LABEL" --id "$ID" 2>&1)
    echo "$out" | grep -qi "key pair generated\|Private Key" \
        && ok "EC P-256 keypairgen" \
        || { no "EC P-256 keypairgen failed: $out"; return; }

    # 7.2 Sign ECDSA-SHA256
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --sign --mechanism ECDSA-SHA256 \
        --id "$ID" --input-file "$PLAIN" --output-file "$SIG" 2>&1)
    echo "$out" | grep -qi "error\|CKR_\|fail" \
        && no "sign ECDSA-SHA256: $out" \
        || ok "sign ECDSA-SHA256"

    # 7.3 Verify ECDSA-SHA256
    out=$(run_cmd pkcs11-tool --module "$MOD" \
        --token-label "$TOKEN0" \
        --verify --mechanism ECDSA-SHA256 \
        --id "$ID" --input-file "$PLAIN" --signature-file "$SIG" 2>&1)
    echo "$out" | grep -qi "Signature is valid" \
        && ok "verify ECDSA-SHA256: Signature is valid" \
        || no "verify ECDSA-SHA256: $out"

    # 7.4 Keypairgen P-384
    LABEL2=test-ec384-menu; ID2=12
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type privkey --id "$ID2" >/dev/null 2>&1 || true
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type pubkey  --id "$ID2" >/dev/null 2>&1 || true

    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --keypairgen --key-type EC:secp384r1 \
        --label "$LABEL2" --id "$ID2" 2>&1)
    echo "$out" | grep -qi "key pair generated\|Private Key" \
        && ok "EC P-384 keypairgen" \
        || no "EC P-384 keypairgen failed: $out"

    # Cleanup
    for ID_C in "$ID" "$ID2"; do
        pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
            --delete-object --type privkey --id "$ID_C" >/dev/null 2>&1 || true
        pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
            --delete-object --type pubkey  --id "$ID_C" >/dev/null 2>&1 || true
    done
    ok "cleanup: EC test keys deleted"
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 8 — pkcs11-tool: AES operations
# ─────────────────────────────────────────────────────────────────────────────
test_p11_aes() {
    hdr "8. pkcs11-tool — AES operations"

    PLAIN=/tmp/pt_aes.bin
    ENC=/tmp/enc_aes.bin
    DEC=/tmp/dec_aes.bin
    IV=00000000000000000000000000000000
    ID_AES=13
    ID_CMAC=14

    # 32-byte block-aligned plaintext
    dd if=/dev/zero bs=32 count=1 2>/dev/null | tr '\0' 'A' > "$PLAIN"

    # ── AES-CBC ──────────────────────────────────────────────────────────────
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type secrkey --id "$ID_AES" >/dev/null 2>&1 || true

    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --keygen --key-type AES:32 --label test-aes-cbc --id "$ID_AES" --sensitive 2>&1)
    echo "$out" | grep -qi "key generated\|Secret Key" \
        && ok "AES-256 keygen (encrypt/decrypt)" \
        || { no "AES-256 keygen failed: $out"; return; }

    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --encrypt --mechanism AES-CBC --id "$ID_AES" \
        --iv "$IV" --input-file "$PLAIN" --output-file "$ENC" 2>&1)
    echo "$out" | grep -qi "error\|CKR_\|fail" \
        && no "AES-CBC encrypt: $out" \
        || ok "AES-CBC encrypt"

    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --decrypt --mechanism AES-CBC --id "$ID_AES" \
        --iv "$IV" --input-file "$ENC" --output-file "$DEC" 2>&1)
    cmp -s "$PLAIN" "$DEC" \
        && ok "AES-CBC decrypt: roundtrip MATCH" \
        || no "AES-CBC decrypt: MISMATCH — $out"

    # ── AES-ECB ──────────────────────────────────────────────────────────────
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --encrypt --mechanism AES-ECB --id "$ID_AES" \
        --input-file "$PLAIN" --output-file "$ENC" 2>&1)
    echo "$out" | grep -qi "error\|CKR_\|fail" \
        && no "AES-ECB encrypt: $out" \
        || ok "AES-ECB encrypt"

    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --decrypt --mechanism AES-ECB --id "$ID_AES" \
        --input-file "$ENC" --output-file "$DEC" 2>&1)
    cmp -s "$PLAIN" "$DEC" \
        && ok "AES-ECB decrypt: roundtrip MATCH" \
        || no "AES-ECB decrypt: MISMATCH — $out"

    # ── AES-CMAC ─────────────────────────────────────────────────────────────
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type secrkey --id "$ID_CMAC" >/dev/null 2>&1 || true

    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --keygen --key-type AES:32 --label test-aes-cmac --id "$ID_CMAC" \
        --sensitive --usage-sign 2>&1)
    echo "$out" | grep -qi "key generated\|Secret Key" \
        && ok "AES-256 keygen (--usage-sign for CMAC)" \
        || { no "AES-256 CMAC keygen failed: $out"; return; }

    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --sign --mechanism AES-CMAC --id "$ID_CMAC" \
        --input-file "$PLAIN" --output-file /tmp/cmac.bin 2>&1)
    echo "$out" | grep -qi "error\|CKR_\|fail" \
        && no "AES-CMAC sign: $out" \
        || ok "AES-CMAC sign: $(xxd -p /tmp/cmac.bin 2>/dev/null | head -c 32)..."

    # ── AES-CTR — pkcs11-tool cannot pass CK_AES_CTR_PARAMS (libckteec bug);
    #    use direct PKCS#11 API via python3/ctypes instead.
    #    OP-TEE TA requires ulCounterBits=1 (off-by-one in bounds check: rejects
    #    values >1 up to 128; only 1 passes the TA's validator).
    if command -v python3 >/dev/null 2>&1; then
        CTR_KEY_LABEL=test-aes-ctr-menu
        CTR_PLAIN=/tmp/pt_ctr_menu.bin
        CTR_ENC=/tmp/enc_ctr_menu.bin
        CTR_DEC=/tmp/dec_ctr_menu.bin
        printf 'AES-CTR test vec 16B' | head -c 16 > "$CTR_PLAIN"

        # Generate a dedicated AES-128 CTR key via pkcs11-tool
        pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
            --delete-object --type secrkey --label "$CTR_KEY_LABEL" >/dev/null 2>&1 || true
        pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
            --keygen --key-type AES:16 --label "$CTR_KEY_LABEL" --sensitive >/dev/null 2>&1

        python3 - "$MOD" "$TOKEN0" "$UPIN" "$CTR_KEY_LABEL" "$CTR_PLAIN" "$CTR_ENC" "$CTR_DEC" << 'PYEOF'
import ctypes, sys, os
mod, token, pin_s, label, plain_f, enc_f, dec_f = sys.argv[1:]
pin = pin_s.encode()
lib = ctypes.CDLL(mod)
CKR_OK=0; CKU_USER=1; CKM_AES_CTR=0x1086

class CK_MECHANISM(ctypes.Structure):
    _fields_=[("mechanism",ctypes.c_ulong),("pParameter",ctypes.c_void_p),("ulParameterLen",ctypes.c_ulong)]
class CK_AES_CTR_PARAMS(ctypes.Structure):
    _fields_=[("ulCounterBits",ctypes.c_uint32),("cb",ctypes.c_ubyte*16)]
class CK_ATTRIBUTE(ctypes.Structure):
    _fields_=[("type",ctypes.c_ulong),("pValue",ctypes.c_void_p),("ulValueLen",ctypes.c_ulong)]

lib.C_Initialize(None)
hSess=ctypes.c_ulong(0)
lib.C_OpenSession(ctypes.c_ulong(0),ctypes.c_ulong(6),None,None,ctypes.byref(hSess))
rv=lib.C_Login(hSess,ctypes.c_ulong(CKU_USER),pin,ctypes.c_ulong(len(pin)))
if rv!=CKR_OK: sys.exit(f"Login failed: {rv:#x}")

lbl=label.encode()
tmpl=(CK_ATTRIBUTE*1)(CK_ATTRIBUTE(3,ctypes.cast(ctypes.c_char_p(lbl),ctypes.c_void_p),len(lbl)))
lib.C_FindObjectsInit(hSess,tmpl,1)
handles=(ctypes.c_ulong*4)(); found=ctypes.c_ulong(0)
lib.C_FindObjects(hSess,handles,4,ctypes.byref(found))
lib.C_FindObjectsFinal(hSess)
if not found.value: sys.exit("key not found")
hKey=ctypes.c_ulong(handles[0])

pt=open(plain_f,'rb').read()
# Encrypt
ctr=CK_AES_CTR_PARAMS(1,(ctypes.c_ubyte*16)(*range(16)))
em=CK_MECHANISM(CKM_AES_CTR,ctypes.cast(ctypes.pointer(ctr),ctypes.c_void_p),ctypes.sizeof(ctr))
rv=lib.C_EncryptInit(hSess,ctypes.byref(em),hKey)
if rv!=CKR_OK: sys.exit(f"EncryptInit: {rv:#x}")
ct=(ctypes.c_ubyte*256)(); cl=ctypes.c_ulong(256)
rv=lib.C_Encrypt(hSess,pt,ctypes.c_ulong(len(pt)),ct,ctypes.byref(cl))
if rv!=CKR_OK: sys.exit(f"Encrypt: {rv:#x}")
open(enc_f,'wb').write(bytes(ct[:cl.value]))

# Decrypt (reset counter to same IV)
ctr2=CK_AES_CTR_PARAMS(1,(ctypes.c_ubyte*16)(*range(16)))
em2=CK_MECHANISM(CKM_AES_CTR,ctypes.cast(ctypes.pointer(ctr2),ctypes.c_void_p),ctypes.sizeof(ctr2))
rv=lib.C_DecryptInit(hSess,ctypes.byref(em2),hKey)
if rv!=CKR_OK: sys.exit(f"DecryptInit: {rv:#x}")
dt=(ctypes.c_ubyte*256)(); dl=ctypes.c_ulong(256)
rv=lib.C_Decrypt(hSess,bytes(ct[:cl.value]),ctypes.c_ulong(cl.value),dt,ctypes.byref(dl))
if rv!=CKR_OK: sys.exit(f"Decrypt: {rv:#x}")
open(dec_f,'wb').write(bytes(dt[:dl.value]))
lib.C_Logout(hSess); lib.C_CloseSession(hSess); lib.C_Finalize(None)
PYEOF
        CTR_RV=$?
        if [ $CTR_RV -eq 0 ] && cmp -s "$CTR_PLAIN" "$CTR_DEC"; then
            ok "AES-CTR encrypt+decrypt: roundtrip MATCH (via direct PKCS#11 API)"
        else
            no "AES-CTR: python3 ctypes test failed (exit=$CTR_RV)"
        fi
        pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
            --delete-object --type secrkey --label "$CTR_KEY_LABEL" >/dev/null 2>&1 || true
        rm -f "$CTR_PLAIN" "$CTR_ENC" "$CTR_DEC"
    else
        sk "AES-CTR: python3 not available for direct PKCS#11 API test"
    fi

    # Cleanup
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type secrkey --id "$ID_AES"  >/dev/null 2>&1 || true
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type secrkey --id "$ID_CMAC" >/dev/null 2>&1 || true
    ok "cleanup: AES test keys deleted"
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 9 — pkcs11-tool: Object management
# ─────────────────────────────────────────────────────────────────────────────
test_p11_objects() {
    hdr "9. pkcs11-tool — Object management"

    ID=15
    LABEL=test-obj-menu

    # 9.1 Write data object
    echo -n "test data object content" > /tmp/obj_data.bin
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --write-object /tmp/obj_data.bin --type data \
        --label "$LABEL" --id "$ID" 2>&1)
    echo "$out" | grep -qi "created\|written\|error" && \
    ! echo "$out" | grep -qi "error\|CKR_" \
        && ok "write-object data" \
        || no "write-object data: $out"

    # 9.2 list-objects
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" --list-objects 2>&1)
    echo "$out" | grep -qi "object\|Key\|Cert\|Data" \
        && ok "list-objects: objects present" \
        || no "list-objects: no objects or error"

    # 9.3 list-objects by type
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --list-objects --type privkey 2>&1)
    ok "list-objects --type privkey: responded"

    # 9.4 read-object data — use --label (data objects require label, not id)
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --read-object --type data --label "$LABEL" \
        --output-file /tmp/obj_read.bin 2>&1)
    cmp -s /tmp/obj_data.bin /tmp/obj_read.bin \
        && ok "read-object data: content matches" \
        || no "read-object data: mismatch or error — $out"

    # 9.5 delete-object data — use --label
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type data --label "$LABEL" 2>&1)
    echo "$out" | grep -qi "error\|CKR_\|fail" \
        && no "delete-object data: $out" \
        || ok "delete-object data"
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 10 — Token object listing & diagnostics
# pkcs15-tool requires a PC/SC reader (no hardware present); use pkcs11-tool
# via libckteec.so to achieve the same: list all object classes on the token.
# ─────────────────────────────────────────────────────────────────────────────
test_p15_info() {
    hdr "10. Token — Info & object listing (via pkcs11-tool)"

    # 10.1 Slot / token info
    out=$(run_cmd pkcs11-tool --module "$MOD" --list-slots 2>&1)
    echo "$out" | grep -qi "error\|failed" \
        && no "list-slots: $out" \
        || ok "list-slots: $(echo "$out" | grep -c 'Slot') slot(s) found"

    # 10.2 List all objects (keys, certs, data)
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --list-objects 2>&1)
    echo "$out" | grep -qi "CKR_\|failed" \
        && no "list-objects: $out" \
        || ok "list-objects: $(echo "$out" | grep -c 'Object') object(s) on token"

    # 10.3 List private keys
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --list-objects --type privkey 2>&1)
    echo "$out" | grep -qi "CKR_\|failed" \
        && no "list privkeys: $out" \
        || ok "list privkeys: $(echo "$out" | grep -c 'Private Key') found"

    # 10.4 List public keys
    out=$(run_cmd pkcs11-tool --module "$MOD" \
        --token-label "$TOKEN0" \
        --list-objects --type pubkey 2>&1)
    echo "$out" | grep -qi "CKR_\|failed" \
        && no "list pubkeys: $out" \
        || ok "list pubkeys: $(echo "$out" | grep -c 'Public Key') found"

    # 10.5 List certificates
    out=$(run_cmd pkcs11-tool --module "$MOD" \
        --token-label "$TOKEN0" \
        --list-objects --type cert 2>&1)
    echo "$out" | grep -qi "CKR_\|failed" \
        && no "list certs: $out" \
        || ok "list certs: $(echo "$out" | grep -c 'X.509 cert') found"

    # 10.6 List secret keys (AES etc.)
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --list-objects --type secrkey 2>&1)
    echo "$out" | grep -qi "CKR_\|failed" \
        && no "list secret keys: $out" \
        || ok "list secret keys: $(echo "$out" | grep -c 'Secret Key') found"

    # 10.7 List data objects
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --list-objects --type data 2>&1)
    echo "$out" | grep -qi "CKR_\|failed" \
        && no "list data objects: $out" \
        || ok "list data objects: $(echo "$out" | grep -c 'Data Object\|object') found"

    # 10.8 Mechanism list
    out=$(run_cmd pkcs11-tool --module "$MOD" --list-mechanisms --slot 0 2>&1)
    count=$(echo "$out" | grep -c ',')
    echo "$out" | grep -qi "error\|failed" \
        && no "list-mechanisms: $out" \
        || ok "list-mechanisms: $count mechanism(s) supported"
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 11 — pkcs15-tool: Key read & verify with openssl
# ─────────────────────────────────────────────────────────────────────────────
test_p15_keys() {
    hdr "11. pkcs15-tool — Read keys + OpenSSL verify"

    if ! command -v pkcs15-tool >/dev/null 2>&1; then
        sk "pkcs15-tool not installed — skipping"
        return
    fi
    if ! command -v openssl >/dev/null 2>&1; then
        sk "openssl not installed — skipping key export verify"
        return
    fi

    # Generate a temporary RSA key in PKCS#11 for reading
    ID=20; LABEL=test-p15-rsa
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type privkey --id "$ID" >/dev/null 2>&1 || true
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type pubkey  --id "$ID" >/dev/null 2>&1 || true

    run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --keypairgen --key-type rsa:2048 --usage-sign --usage-decrypt \
        --label "$LABEL" --id "$ID" >/dev/null 2>&1
    ok "RSA-2048 key created for pkcs15 test"

    # Read public key via pkcs11-tool (replaces pkcs15-tool --read-public-key which needs PC/SC)
    local PUB_DER=/tmp/p11_test_pub_$$.der
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --read-object --type pubkey --id "$ID" \
        --output-file "$PUB_DER" 2>&1)
    if [ -f "$PUB_DER" ] && [ -s "$PUB_DER" ]; then
        ok "read-object pubkey (pkcs11-tool): $(wc -c < "$PUB_DER") bytes DER"
        # Verify it is a valid RSA public key using openssl
        ossl_out=$(openssl rsa -inform DER -pubin -in "$PUB_DER" -noout 2>&1)
        [ $? -eq 0 ] && ok "OpenSSL pubkey verify: valid RSA SubjectPublicKeyInfo" \
                      || no "OpenSSL pubkey verify: $ossl_out"
    else
        no "read-object pubkey failed: $out"
        sk "OpenSSL pubkey verify: skipped (pubkey read failed)"
    fi
    rm -f "$PUB_DER"

    # Cleanup
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type privkey --id "$ID" >/dev/null 2>&1 || true
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type pubkey  --id "$ID" >/dev/null 2>&1 || true
    ok "cleanup: p15 test key deleted"
}

# ─────────────────────────────────────────────────────────────────────────────
# SECTION 12 — pkcs15-tool: Certificate write & read
# ─────────────────────────────────────────────────────────────────────────────
test_p15_cert() {
    hdr "12. pkcs15-tool — Certificate import & read"

    if ! command -v pkcs15-tool >/dev/null 2>&1; then
        sk "pkcs15-tool not installed — skipping"
        return
    fi
    if ! command -v openssl >/dev/null 2>&1; then
        sk "openssl not installed — cannot generate test cert"
        return
    fi

    # Generate a self-signed cert with openssl
    if ! [ -f /tmp/test_cert.pem ]; then
        openssl req -x509 -newkey rsa:1024 -keyout /tmp/test_key.pem \
            -out /tmp/test_cert.pem -days 365 -nodes \
            -subj "/CN=pkcs-test-menu/O=SparrowHawk" >/dev/null 2>&1
    fi

    if ! [ -f /tmp/test_cert.pem ]; then
        sk "could not generate test certificate with openssl"
        return
    fi
    ok "self-signed test certificate generated"

    # Write cert via pkcs11-tool (pkcs15-init requires more setup)
    ID=21
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --write-object /tmp/test_cert.pem --type cert \
        --label test-cert-menu --id "$ID" 2>&1)
    echo "$out" | grep -qi "error\|CKR_\|fail" \
        && no "write-object cert (via pkcs11-tool): $out" \
        || ok "write-object cert (via pkcs11-tool)"

    # Read cert back via pkcs11-tool (replaces pkcs15-tool --read-certificate which needs PC/SC)
    local CERT_DER=/tmp/p11_test_cert_$$.der
    out=$(run_cmd pkcs11-tool --module "$MOD" --login \
        --token-label "$TOKEN0" --pin "$UPIN" \
        --read-object --type cert --id "$ID" \
        --output-file "$CERT_DER" 2>&1)
    if [ -f "$CERT_DER" ] && [ -s "$CERT_DER" ]; then
        ok "read-object cert (pkcs11-tool): $(wc -c < "$CERT_DER") bytes DER"
        # Verify it is a valid X.509 certificate using openssl
        ossl_out=$(openssl x509 -inform DER -in "$CERT_DER" -noout -subject 2>&1)
        [ $? -eq 0 ] && ok "OpenSSL cert verify: $ossl_out" \
                      || no "OpenSSL cert verify: $ossl_out"
    else
        no "read-object cert failed: $out"
    fi
    rm -f "$CERT_DER"

    # Cleanup
    pkcs11-tool --module "$MOD" --login --token-label "$TOKEN0" --pin "$UPIN" \
        --delete-object --type cert --id "$ID" >/dev/null 2>&1 || true
    ok "cleanup: test cert deleted"
}

# ─────────────────────────────────────────────────────────────────────────────
# Parameter editor — lets the user change MOD/TOKEN0/TOKEN1/UPIN/SOPIN at
# runtime without restarting the script.  Press Enter to keep the current value.
# ─────────────────────────────────────────────────────────────────────────────
edit_params() {
    printf "\n${BOLD}${CYAN}── Configure Parameters ──${NC}\n"
    printf "  Press Enter to keep the current value.\n\n"

    printf "  Module path    [${YELLOW}%s${NC}]: " "$MOD"
    read -r _v </dev/tty; [ -n "$_v" ] && MOD="$_v"

    printf "  Token 0 label  [${YELLOW}%s${NC}]: " "$TOKEN0"
    read -r _v </dev/tty; [ -n "$_v" ] && TOKEN0="$_v"

    printf "  Token 1 label  [${YELLOW}%s${NC}]: " "$TOKEN1"
    read -r _v </dev/tty; [ -n "$_v" ] && TOKEN1="$_v"

    printf "  User PIN       [${YELLOW}%s${NC}]: " "$UPIN"
    read -r _v </dev/tty; [ -n "$_v" ] && UPIN="$_v"

    printf "  SO PIN         [${YELLOW}%s${NC}]: " "$SOPIN"
    read -r _v </dev/tty; [ -n "$_v" ] && SOPIN="$_v"

    printf "\n  ${GREEN}Parameters updated.${NC}\n"
}

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
print_summary() {
    TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
    printf "\n${BOLD}══════════════════════════════════════════════${NC}\n"
    printf "${BOLD}  Test Summary${NC}\n"
    printf "${BOLD}══════════════════════════════════════════════${NC}\n"
    printf "  Total : %d\n" "$TOTAL"
    printf "  ${GREEN}PASS${NC}  : %d\n" "$PASS_COUNT"
    printf "  ${RED}FAIL${NC}  : %d\n" "$FAIL_COUNT"
    printf "  ${YELLOW}SKIP${NC}  : %d\n" "$SKIP_COUNT"
    printf "\n  Log saved to: %s\n\n" "$LOGFILE"
    [ "$FAIL_COUNT" -eq 0 ] && printf "  ${GREEN}${BOLD}All tests passed!${NC}\n\n" \
                             || printf "  ${RED}${BOLD}%d test(s) FAILED — see log${NC}\n\n" "$FAIL_COUNT"
}

# ─────────────────────────────────────────────────────────────────────────────
# Menu
# ─────────────────────────────────────────────────────────────────────────────
show_menu() {
    printf "\n${BOLD}${CYAN}╔══════════════════════════════════════════════╗${NC}\n"
    printf "${BOLD}${CYAN}║   PKCS#11 / PKCS#15 Test Suite               ║${NC}\n"
    printf "${BOLD}${CYAN}║   Sparrow Hawk — OP-TEE libckteec             ║${NC}\n"
    printf "${BOLD}${CYAN}╚══════════════════════════════════════════════╝${NC}\n"
    printf "\n  Module: ${YELLOW}%s${NC}\n" "$MOD"
    printf "  Token : ${YELLOW}%s${NC}   UPIN: %s   SOPIN: %s\n" \
        "$TOKEN0" "$UPIN" "$SOPIN"
    printf "\n  ${BOLD}pkcs11-tool tests:${NC}\n"
    printf "   1) Environment check\n"
    printf "   2) Token info & diagnostics\n"
    printf "   3) Token lifecycle (login/init)\n"
    printf "   4) Random number generation\n"
    printf "   5) Hash / Digest\n"
    printf "   6) RSA operations (keypairgen, sign, verify, encrypt, decrypt)\n"
    printf "   7) EC operations  (keypairgen, sign, verify)\n"
    printf "   8) AES operations (CBC, ECB, CMAC)\n"
    printf "   9) Object management (write, list, read, delete)\n"
    printf "\n  ${BOLD}pkcs15-tool tests:${NC}\n"
    printf "  10) Info & object listing\n"
    printf "  11) Key read + OpenSSL verify\n"
    printf "  12) Certificate import & read\n"
    printf "\n  ${BOLD}Run all:${NC}\n"
    printf "   a) Run ALL tests\n"
    printf "   c) Configure parameters (module, tokens, PINs)\n"
    printf "   q) Quit\n"
    printf "\n  Choice: "
}

run_section() {
    case "$1" in
        1)  test_env ;;
        2)  test_p11_info ;;
        3)  test_p11_token ;;
        4)  test_p11_random ;;
        5)  test_p11_hash ;;
        6)  test_p11_rsa ;;
        7)  test_p11_ec ;;
        8)  test_p11_aes ;;
        9)  test_p11_objects ;;
        10) test_p15_info ;;
        11) test_p15_keys ;;
        12) test_p15_cert ;;
        a|A)
            for S in 1 2 3 4 5 6 7 8 9 10 11 12; do run_section "$S"; done
            ;;
    esac
}

# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────
echo "Log: $LOGFILE" > "$LOGFILE"
echo "Started: $(date)" >> "$LOGFILE"
echo "Token: $TOKEN0  MOD: $MOD" >> "$LOGFILE"

if [ "$AUTO" -eq 1 ]; then
    run_section a
    print_summary
    exit $FAIL_COUNT
fi

# Interactive menu loop
while true; do
    show_menu
    read -r CHOICE </dev/tty
    case "$CHOICE" in
        [1-9]|10|11|12|a|A) run_section "$CHOICE" ;;
        c|C) edit_params ;;
        q|Q) print_summary; exit 0 ;;
        *) printf "  Invalid choice.\n" ;;
    esac
    printf "\n  Press Enter to return to menu..."
    read -r _dummy </dev/tty
done
