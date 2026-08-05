printf '#!/bin/sh\n' > /usr/bin/pkcs11-rsa-sign-recover.sh
printf 'SLOT=${1:-1} PIN=${2:-87654321}\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf 'P11="pkcs11-tool --module /usr/lib/libckteec.so --slot $SLOT --login --pin $PIN"\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf 'T=/tmp/pkcs11-rsa\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf 'ok() { echo "[PASS] $1"; }; ko() { echo "[FAIL] $1"; exit 1; }\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf 'trap "$P11 --delete-object --type privkey --label rsa-t 2>/dev/null; $P11 --delete-object --type pubkey --label rsa-t 2>/dev/null; rm -f $T.*" EXIT\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf '$P11 --keypairgen --key-type RSA:2048 --label rsa-t --id bb --usage-sign 2>&1 | grep -q "Key pair generated" && ok "keygen (TEE, never extractable)" || ko "keygen"\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf '$P11 --read-object --type pubkey --label rsa-t --output-file $T.der 2>/dev/null && openssl rsa -inform DER -pubin -in $T.der -out $T.pem 2>/dev/null && ok "pubkey exported" || ko "pubkey export"\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf 'printf "Secure TEE message: Sparrow Hawk RSA test" > $T.plain\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf '$P11 --sign --mechanism RSA-PKCS --label rsa-t --input-file $T.plain --output-file $T.sig 2>/dev/null && ok "signed with TEE private key" || ko "sign"\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf 'openssl pkeyutl -verifyrecover -pubin -inkey $T.pem -in $T.sig -out $T.recv 2>/dev/null && ok "recovered with public key (openssl)" || ko "recover"\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
printf '[ "$(cat $T.plain)" = "$(cat $T.recv)" ] && ok "ROUND-TRIP OK: $(cat $T.recv)" || ko "mismatch"\n' >> /usr/bin/pkcs11-rsa-sign-recover.sh
chmod +x /usr/bin/pkcs11-rsa-sign-recover.sh
echo "Script deployed OK"
