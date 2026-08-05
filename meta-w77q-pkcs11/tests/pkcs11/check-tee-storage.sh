#!/bin/sh
# check-tee-storage.sh — Inspect OP-TEE secure storage (REE FS) on Sparrow Hawk
#
# Shows: TEE device status, /data/tee/ directory layout, per-TA object counts,
# storage sizes, PKCS#11 token/object inventory, and supplicant mount info.
#
# Usage: check-tee-storage.sh [--pkcs11-pin <pin>] [--token-label <label>]

set -e

MODULE=/usr/lib/libckteec.so
TEE_STORAGE=/data/tee

# Well-known TA UUID → name mapping (Sparrow Hawk image)
# Format: "<uuid> <name>"
TA_NAMES="
8aaaf200-2450-11e4-abe2-0002a5d5c51b hello_world
5dbac793-f574-4871-8ad3-04331ec17f24 aes
a734eed9-d6a1-4244-aa50-7c99719e7b7b acipher
50c82425-1e9b-4703-b976-6e7e4a6f4e68 sha
b6c53aba-9669-4668-a7f2-205629d00f86 random
f4e750bb-1437-4fbf-8785-8d3580c34994 secure_storage
484d4143-4f50-2d49-3050-534e6150364b hotp
1dc6a16b-8b42-4d18-b0fe-2b3fa4f42a03 ecdh
1945e8e7-a445-5f16-5bc2-74a00cc5d71b ecdsa
2a287631-de99-4af3-8d64-c558cbf59310 sign_verify
f066f150-b571-4a98-9db8-e72c0ef4e99f plugins
fd02c9da-306c-48c7-a49c-bbd827ae86ee pkcs11_ta
"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

hdr()      { echo; echo "${BOLD}${CYAN}━━━ $1 ━━━${NC}"; }
info()     { echo "  $1"; }
run_show() { printf "\n  ${CYAN}▶${NC} %s\n" "$*" >&2; eval "$*" 2>&1 | tee /dev/tty; }

# Parse options
PKCS11_PIN=""
TOKEN_LABEL="my-token"
while [ $# -gt 0 ]; do
    case "$1" in
        --pkcs11-pin)   PKCS11_PIN="$2";   shift 2 ;;
        --token-label)  TOKEN_LABEL="$2";  shift 2 ;;
        *) shift ;;
    esac
done

echo "${BOLD}════════════════════════════════════════${NC}"
echo "${BOLD} OP-TEE Secure Storage Info${NC}"
echo "${BOLD}════════════════════════════════════════${NC}"

# ── 1. TEE Device & Driver ───────────────────────────────────────────────────

hdr "TEE Device"

for dev in /dev/tee0 /dev/teepriv0; do
    if [ -c "$dev" ]; then
        info "${GREEN}✔${NC} $dev  $(ls -la $dev | awk '{print $1, $3, $4}')"
    else
        info "✘ $dev  (missing)"
    fi
done

if [ -f /sys/bus/platform/drivers/optee/*/tee_version 2>/dev/null ]; then
    ver=$(cat /sys/bus/platform/drivers/optee/*/tee_version 2>/dev/null)
    info "OP-TEE kernel driver version : $ver"
fi

optee_line=$(dmesg 2>/dev/null | grep -i "optee.*version" | tail -1)
[ -n "$optee_line" ] && info "Boot: $optee_line"

# ── 2. tee-supplicant ────────────────────────────────────────────────────────

hdr "tee-supplicant"

if systemctl is-active --quiet optee.service 2>/dev/null; then
    info "${GREEN}✔${NC} optee.service is active"
    systemctl show optee.service --no-pager \
        --property=MainPID,ExecMainStartTimestamp,MemoryCurrent \
        2>/dev/null | sed 's/^/    /'
elif pid=$(pgrep tee-supplicant 2>/dev/null | head -1); then
    info "${GREEN}✔${NC} tee-supplicant running (PID $pid)"
    cat /proc/$pid/cmdline 2>/dev/null | tr '\0' ' ' | sed 's/^/    cmdline: /'
    echo
else
    info "✘ tee-supplicant is NOT running"
fi

# ── 3. REE FS storage root ───────────────────────────────────────────────────

hdr "REE Filesystem Storage  ($TEE_STORAGE)"

if [ ! -d "$TEE_STORAGE" ]; then
    info "Directory does not exist — no objects stored yet"
else
    # Filesystem where /data/tee lives
    fs_info=$(df -h "$TEE_STORAGE" 2>/dev/null | tail -1)
    info "Filesystem : $fs_info"

    total_bytes=$(du -sb "$TEE_STORAGE" 2>/dev/null | awk '{print $1}')
    total_human=$(du -sh "$TEE_STORAGE" 2>/dev/null | awk '{print $1}')
    total_files=$(find "$TEE_STORAGE" -type f 2>/dev/null | wc -l)
    info "Total size : ${total_human}  (${total_files} encrypted blob(s))"

    echo
    info "Directory layout:"
    # Show top-level entries; try to resolve UUID directories to TA names
    find "$TEE_STORAGE" -maxdepth 1 -mindepth 1 2>/dev/null | sort | while read entry; do
        base=$(basename "$entry")
        name=""
        # Check if this entry looks like a TA UUID
        echo "$TA_NAMES" | while read uuid ta_name; do
            [ -z "$uuid" ] && continue
            if [ "$base" = "$uuid" ]; then
                printf "    %s  ${YELLOW}(%s)${NC}\n" "$base" "$ta_name"
                return
            fi
        done
        # Fallback: just print with size
        sz=$(du -sh "$entry" 2>/dev/null | awk '{print $1}')
        matched=$(echo "$TA_NAMES" | awk -v u="$base" '$1==u {print $2}')
        if [ -n "$matched" ]; then
            printf "    ${GREEN}%-45s${NC}  ${YELLOW}%-20s${NC}  %s\n" \
                "$base" "[$matched]" "$sz"
        else
            printf "    %-45s  %s\n" "$base" "$sz"
        fi
        # Count objects inside
        if [ -d "$entry" ]; then
            n=$(find "$entry" -type f 2>/dev/null | wc -l)
            printf "    %45s  └─ %d object(s)\n" "" "$n"
        fi
    done

    echo
    info "Per-TA storage breakdown:"
    printf "    %-45s  %-20s  %8s  %s\n" "TA UUID" "Name" "Size" "Files"
    printf "    %-45s  %-20s  %8s  %s\n" \
        "─────────────────────────────────────────────" \
        "──────────────────" "────────" "─────"
    echo "$TA_NAMES" | while read uuid ta_name; do
        [ -z "$uuid" ] && continue
        ta_dir="$TEE_STORAGE/$uuid"
        if [ -d "$ta_dir" ]; then
            sz=$(du -sh "$ta_dir" 2>/dev/null | awk '{print $1}')
            nf=$(find "$ta_dir" -type f 2>/dev/null | wc -l)
            printf "    ${GREEN}%-45s${NC}  %-20s  %8s  %d\n" \
                "$uuid" "$ta_name" "$sz" "$nf"
        fi
    done

    # Also show any unknown UUID directories
    find "$TEE_STORAGE" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort | while read d; do
        base=$(basename "$d")
        known=$(echo "$TA_NAMES" | awk -v u="$base" '$1==u {print 1}')
        if [ -z "$known" ]; then
            sz=$(du -sh "$d" 2>/dev/null | awk '{print $1}')
            nf=$(find "$d" -type f 2>/dev/null | wc -l)
            printf "    %-45s  %-20s  %8s  %d\n" "$base" "(unknown TA)" "$sz" "$nf"
        fi
    done

    # Flat-file objects (not in subdirectory) at storage root
    flat=$(find "$TEE_STORAGE" -maxdepth 1 -type f 2>/dev/null | wc -l)
    [ "$flat" -gt 0 ] && info "Flat objects at root: $flat file(s)"
fi

# ── 4. Installed TAs ─────────────────────────────────────────────────────────

hdr "Installed Trusted Applications  (/usr/lib/optee_armtz)"

TA_DIR=/usr/lib/optee_armtz
if [ -d "$TA_DIR" ]; then
    ta_count=$(find "$TA_DIR" -name "*.ta" 2>/dev/null | wc -l)
    info "Found ${ta_count} TA(s):"
    printf "    %-45s  %-24s  %s\n" "UUID" "Name" "Size"
    printf "    %-45s  %-24s  %s\n" \
        "─────────────────────────────────────────────" \
        "────────────────────────" "────────"
    find "$TA_DIR" -name "*.ta" 2>/dev/null | sort | while read ta_file; do
        uuid=$(basename "$ta_file" .ta)
        sz=$(du -sh "$ta_file" 2>/dev/null | awk '{print $1}')
        name=$(echo "$TA_NAMES" | awk -v u="$uuid" '$1==u {print $2}')
        [ -z "$name" ] && name="(unknown)"
        printf "    %-45s  %-24s  %s\n" "$uuid" "$name" "$sz"
    done
else
    info "TA directory not found: $TA_DIR"
fi

# ── 5. PKCS#11 Token Info ────────────────────────────────────────────────────

hdr "PKCS#11 Token Info"

if ! command -v pkcs11-tool >/dev/null 2>&1; then
    info "pkcs11-tool not found (install opensc)"
elif [ ! -f "$MODULE" ]; then
    info "libckteec not found: $MODULE"
else
    run_show pkcs11-tool --module '"$MODULE"' --list-slots
    echo

    # List objects if a token label + PIN are provided
    if [ -n "$PKCS11_PIN" ]; then
        echo
        info "Objects in token '${TOKEN_LABEL}':"
        run_show pkcs11-tool --module '"$MODULE"' --login \
            --token-label '"$TOKEN_LABEL"' --pin '"$PKCS11_PIN"' \
            --list-objects || info "  (token not initialized or wrong PIN)"
    else
        echo
        info "Tip: pass --pkcs11-pin <pin> [--token-label <label>] to list token objects"
    fi
fi

# ── 6. Storage library versions ──────────────────────────────────────────────

hdr "OP-TEE Library Versions"

for lib in /usr/lib/libteec.so.2* /usr/lib/libckteec.so.*.*; do
    [ -f "$lib" ] || continue
    printf "  %-40s" "$(basename $lib)"
    readelf -d "$lib" 2>/dev/null | grep SONAME | awk '{print $5}' | tr -d '[]'
done

echo
echo "${BOLD}════════════════════════════════════════${NC}"
echo "${BOLD} Done${NC}"
echo "${BOLD}════════════════════════════════════════${NC}"
