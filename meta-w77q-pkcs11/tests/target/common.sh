#!/bin/sh
# Common helpers for on-target W77Q/TEE/PKCS#11 test scripts.
# Deploy as /usr/bin/tee-tests-common.sh on the board.
case "$1" in --help|-h)
    echo "Usage: . $(basename $0)  (source this file, do not run directly)"
    echo ""
    echo "  Shared helper library for W77Q/TEE/PKCS#11 on-target test scripts."
    echo "  Exported vars: MOD, TOKEN0, TOKEN1, SOPIN, UPIN, PASS"
    echo "  Exported fns:  die, ensure_tee_supplicant, ensure_token, del_obj, section, pass, fail"
    exit 0 ;;
esac

MOD=/usr/lib/libckteec.so
[ -e "$MOD" ] || MOD=/usr/lib/libckteec.so.0

TOKEN0=optee-test-token   # slot 0
TOKEN1=yuri               # slot 1
SOPIN=12345678
UPIN=87654321
PASS=0

die() { echo "FAIL: $*" >&2; exit 1; }

ensure_tee_supplicant() {
    if ! systemctl is-active tee-supplicant -q 2>/dev/null; then
        echo "  starting tee-supplicant..."
        systemctl start tee-supplicant
        sleep 2
    else
        echo "  tee-supplicant OK"
    fi
}

ensure_token() {
    local token="$1" slot="$2"
    if ! pkcs11-tool --module "$MOD" -T 2>&1 | grep -q "token label.*:.*$token"; then
        echo "  initialising token '$token' on slot $slot..."
        pkcs11-tool --module "$MOD" --slot-index "$slot" \
            --init-token --label "$token" --so-pin "$SOPIN" 2>&1
        pkcs11-tool --module "$MOD" --token-label "$token" \
            --login --login-type so --so-pin "$SOPIN" \
            --init-pin --new-pin "$UPIN" 2>&1
        echo "  token '$token' initialised"
    elif ! pkcs11-tool --module "$MOD" --token-label "$token" \
            --login --pin "$UPIN" -O >/dev/null 2>&1; then
        echo "  token '$token' exists but PIN not set -- initialising user PIN..."
        pkcs11-tool --module "$MOD" --token-label "$token" \
            --login --login-type so --so-pin "$SOPIN" \
            --init-pin --new-pin "$UPIN" 2>&1
        echo "  token '$token' PIN ready"
    else
        echo "  token '$token' OK"
    fi
}

del_obj() {
    # del_obj TOKEN TYPE LABEL
    pkcs11-tool --module "$MOD" --token-label "$1" --login --pin "$UPIN" \
        --delete-object --type "$2" --label "$3" 2>/dev/null || true
}

section() { echo; echo "== $* =="; }
pass()    { echo "  [PASS] $*"; }
fail()    { echo "  [FAIL] $*"; PASS=0; }
