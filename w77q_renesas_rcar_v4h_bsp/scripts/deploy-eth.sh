#!/bin/bash
# deploy-eth.sh — Deploy updated binaries to Sparrow Hawk board over Ethernet
# ============================================================================
# Copies freshly-built host apps and TA binaries to the board via SSH/SCP.
# Does NOT reflash SD/MMC — use flash-mmc.sh for full image updates.
#
# Usage:
#   ./scripts/deploy-eth.sh --board 192.168.1.100 --all
#   ./scripts/deploy-eth.sh --board 192.168.1.100 --app tee-demo
#   ./scripts/deploy-eth.sh --board 192.168.1.100 --app pkcs11-demo --app optee-examples
#   ./scripts/deploy-eth.sh --board 192.168.1.100 --all --restart-supplicant --verify
#
# Prerequisites on board: openssh-server (included by default).
# Find board IP: run `ip addr show eth0` on serial console, or check router DHCP.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
WORK_DIR="$REPO_ROOT/build/build-sparrow-hawk/tmp/work"

info()  { echo -e "\033[1;36m[deploy-eth]\033[0m $*"; }
ok()    { echo -e "\033[1;32m[   ok   ]\033[0m $*"; }
warn()  { echo -e "\033[1;33m[  warn  ]\033[0m $*"; }
die()   { echo -e "\033[1;31m[  FAIL  ]\033[0m $*" >&2; exit 1; }

usage() {
    echo "Usage: $0 --board <ip> [options]"
    echo ""
    echo "  --board <ip|host>        Board IP or hostname (prompted if omitted)"
    echo "  --app tee-demo           Deploy tee-demo host app + TA"
    echo "  --app pkcs11-demo        Deploy pkcs11-demo host app"
    echo "  --app optee-examples     Deploy all optee-examples binaries + TAs"
    echo "  --all                    Deploy all apps (tee-demo + pkcs11-demo + optee-examples)"
    echo "  --restart-supplicant     Restart tee-supplicant on board after deploy"
    echo "  --verify                 Run quick sanity test after deploy"
    echo "  -h | --help              Show this help"
    exit 0
}

BOARD=""
APPS=()
DEPLOY_ALL=no
RESTART_SUPP=no
DO_VERIFY=no

while [[ $# -gt 0 ]]; do
    case "$1" in
        --board)     BOARD="$2"; shift 2 ;;
        --board=*)   BOARD="${1#*=}"; shift ;;
        --app)       APPS+=("$2"); shift 2 ;;
        --all)       DEPLOY_ALL=yes; shift ;;
        --restart-supplicant) RESTART_SUPP=yes; shift ;;
        --verify)    DO_VERIFY=yes; shift ;;
        -h|--help)   usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

# ── Board address ─────────────────────────────────────────────────────────────
if [[ -z "$BOARD" ]]; then
    read -rp "$(echo -e "\033[1;33mEnter board IP or hostname: \033[0m")" BOARD
fi

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes"
SSH="ssh $SSH_OPTS root@$BOARD"
SCP="scp $SSH_OPTS"

# ── Reachability check ────────────────────────────────────────────────────────
info "Checking board at $BOARD ..."
if ! ping -c1 -W3 "$BOARD" &>/dev/null; then
    die "Cannot reach $BOARD — check Ethernet cable and board IP.\n  On board serial: ip addr show eth0"
fi
if ! $SSH true 2>/dev/null; then
    die "SSH to root@$BOARD failed — is openssh-server running? Try: picocom -b 921600 /dev/ttyUSB0"
fi
ok "Board reachable: root@$BOARD"

# ── Helpers ───────────────────────────────────────────────────────────────────
scp_file() {
    local src="$1" dst="$2"
    if [[ ! -f "$src" ]]; then
        warn "  Not found (skip): $src"
        return
    fi
    info "  → $(basename "$src")"
    $SCP "$src" "root@$BOARD:$dst"
}

scp_glob() {
    local pattern="$1" dst="$2"
    local found=0
    for f in $pattern; do
        [[ -f "$f" ]] || continue
        info "  → $(basename "$f")"
        $SCP "$f" "root@$BOARD:$dst"
        found=1
    done
    [[ $found -eq 0 ]] && warn "  No files matched: $pattern"
}

# ── Deployment functions ──────────────────────────────────────────────────────

deploy_tee_demo() {
    local work
    work=$(find "$WORK_DIR/cortexa76-poky-linux/tee-demo" -maxdepth 1 -type d \
           | sort -V | tail -1)
    [[ -d "$work" ]] || die "tee-demo work dir not found — did you build tee-demo?"
    work="$work/image"
    info "Deploying tee-demo from $work ..."
    scp_file "$work/usr/bin/tee-demo" "/usr/bin/tee-demo"
    scp_glob "$work/usr/lib/optee_armtz/b1c2d3e4-*.ta" "/lib/optee_armtz/"
    ok "tee-demo deployed."
}

deploy_pkcs11_demo() {
    local work
    work=$(find "$WORK_DIR"/{cortexa76,sparrow_hawk}-poky-linux/pkcs11-app \
           -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1)
    if [[ -z "$work" ]] || [[ ! -d "$work" ]]; then
        die "pkcs11-app work dir not found — did you build pkcs11-app?"
    fi
    work="$work/image"
    info "Deploying pkcs11-demo from $work ..."
    scp_file "$work/usr/bin/pkcs11-demo" "/usr/bin/pkcs11-demo"
    ok "pkcs11-demo deployed."
}

deploy_optee_examples() {
    local work
    work=$(find "$WORK_DIR"/{cortexa76,sparrow_hawk}-poky-linux/optee-examples \
           -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1)
    if [[ -z "$work" ]] || [[ ! -d "$work" ]]; then
        die "optee-examples work dir not found — did you build optee-examples?"
    fi
    work="$work/image"
    info "Deploying optee-examples from $work ..."
    scp_glob "$work/usr/bin/optee_example_*" "/usr/bin/"
    scp_glob "$work/lib/optee_armtz/*.ta"   "/lib/optee_armtz/"
    ok "optee-examples deployed."
}

# ── Main dispatch ─────────────────────────────────────────────────────────────
if [[ "$DEPLOY_ALL" == "yes" ]]; then
    APPS=(tee-demo pkcs11-demo optee-examples)
fi

if [[ ${#APPS[@]} -eq 0 ]]; then
    warn "No apps specified. Use --app <name> or --all."
    usage
fi

for app in "${APPS[@]}"; do
    case "$app" in
        tee-demo)        deploy_tee_demo ;;
        pkcs11-demo)     deploy_pkcs11_demo ;;
        optee-examples)  deploy_optee_examples ;;
        *) warn "Unknown app: $app (valid: tee-demo, pkcs11-demo, optee-examples)" ;;
    esac
done

# ── Restart tee-supplicant ────────────────────────────────────────────────────
if [[ "$RESTART_SUPP" == "yes" ]]; then
    info "Restarting tee-supplicant on board..."
    $SSH "killall tee-supplicant 2>/dev/null || true; sleep 1; tee-supplicant &>/dev/null &"
    sleep 2
    ok "tee-supplicant restarted."
fi

# ── Verify ────────────────────────────────────────────────────────────────────
if [[ "$DO_VERIFY" == "yes" ]]; then
    info "Running quick verification on board..."
    echo ""

    for app in "${APPS[@]}"; do
        case "$app" in
            tee-demo)
                info "  Verifying tee-demo (hello world TA)..."
                result=$($SSH "optee_example_hello_world 2>&1" || true)
                if echo "$result" | grep -q "TA incremented"; then
                    ok "  tee-demo OP-TEE: hello_world TA OK"
                else
                    warn "  tee-demo OP-TEE: unexpected output — $result"
                fi
                ;;
            pkcs11-demo)
                info "  Verifying pkcs11-demo..."
                result=$($SSH "pkcs11-demo 2>&1; echo exit:\$?" || true)
                if echo "$result" | grep -q "exit:0"; then
                    ok "  pkcs11-demo: PASS"
                else
                    warn "  pkcs11-demo: check output — $result"
                fi
                ;;
        esac
    done
fi

echo ""
ok "deploy-eth complete → root@$BOARD"
