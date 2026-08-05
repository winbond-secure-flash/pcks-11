#!/bin/sh
# pkcs11-rsa-sign-recover.sh [slot] [pin]
# TEE: generate RSA-2048, sign with private key → recover with public key (openssl).
SLOT=${1:-1} PIN=${2:-87654321}
P11="pkcs11-tool --module /usr/lib/libckteec.so --slot $SLOT --login --pin $PIN"
T=/tmp/pkcs11-rsa

CYAN='\033[0;36m'; GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'

ok() { printf "${GREEN}[PASS]${NC} %s\n" "$1"; }
ko() { printf "${RED}[FAIL]${NC} %s\n" "$1"; exit 1; }

# Show command and output on terminal; return real exit code
r() {
    printf "${CYAN}▶${NC} %s\n" "$*" >&2
    _t=$(mktemp)
    eval "$@" >"$_t" 2>&1
    _rc=$?
    sed 's/^/  /' "$_t" >&2
    rm -f "$_t"
    return $_rc
}

# Like r() but also pipes stdout through (for | grep callers)
r_out() {
    printf "${CYAN}▶${NC} %s\n" "$*" >&2
    eval "$@" 2>&1 | tee /dev/tty
}

trap "r $P11 --delete-object --type privkey --label rsa-t 2>/dev/null
      r $P11 --delete-object --type pubkey  --label rsa-t 2>/dev/null
      rm -f $T.*" EXIT

r_out $P11 --keypairgen --key-type RSA:2048 --label rsa-t --id bb --usage-sign \
  | grep -q "Key pair generated" && ok "keygen (TEE, never extractable)" || ko "keygen"

r_out $P11 --read-object --type pubkey --label rsa-t --output-file $T.der
r openssl rsa -inform DER -pubin -in $T.der -out $T.pem \
  && ok "pubkey exported" || ko "pubkey export"

printf "Secure TEE message: Sparrow Hawk RSA test" > $T.plain
printf "\n${CYAN}▶${NC} printf ... > %s\n  plaintext: %s\n" "$T.plain" "$(cat $T.plain)"

r_out $P11 --sign --mechanism RSA-PKCS --label rsa-t \
     --input-file $T.plain --output-file $T.sig \
  && ok "signed with TEE private key ($(wc -c < $T.sig) bytes)" || ko "sign"

r openssl pkeyutl -verifyrecover -pubin -inkey $T.pem \
                  -in $T.sig -out $T.recv \
  && ok "recovered with public key (openssl, userspace)" || ko "recover"

printf "\n  recovered: %s\n" "$(cat $T.recv)"
[ "$(cat $T.plain)" = "$(cat $T.recv)" ] \
  && ok "ROUND-TRIP OK: $(cat $T.recv)" || ko "mismatch"
