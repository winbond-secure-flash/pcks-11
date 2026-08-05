#!/bin/bash
# Wrapper -- delegates to meta-w77q-pkcs11/build.sh
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR/meta-w77q-pkcs11" && bash build.sh "$@"
