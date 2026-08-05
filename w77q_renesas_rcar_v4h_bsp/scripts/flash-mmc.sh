#!/bin/bash
# flash-mmc.sh — Flash a Sparrow Hawk WIC image to an SD/MMC card
# ================================================================
# Uses bmaptool when available (fast sparse flash), falls back to dd.
#
# Usage:
#   ./scripts/flash-mmc.sh --device /dev/sdb
#   ./scripts/flash-mmc.sh --device /dev/sdb --weston
#   ./scripts/flash-mmc.sh --list            # list candidate devices

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
DEPLOY_DIR="$REPO_ROOT/build/build-sparrow-hawk/tmp/deploy/images/sparrow-hawk"

info()  { echo -e "\033[1;36m[flash-mmc]\033[0m $*"; }
ok()    { echo -e "\033[1;32m[  ok  ]\033[0m $*"; }
warn()  { echo -e "\033[1;33m[ warn ]\033[0m $*"; }
die()   { echo -e "\033[1;31m[ FAIL ]\033[0m $*" >&2; exit 1; }

usage() {
    echo "Usage: $0 [options]"
    echo "  --device /dev/sdX     Target block device (prompted if omitted)"
    echo "  --weston              Use core-image-weston instead of core-image-minimal"
    echo "  --image /path/to.wic.gz  Override image path"
    echo "  --list                List candidate removable devices and exit"
    echo "  --no-eject            Skip final eject"
    echo "  -h | --help           Show this help"
    exit 0
}

DEVICE=""
USE_WESTON=no
CUSTOM_IMAGE=""
LIST_ONLY=no
DO_EJECT=yes

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device)     DEVICE="$2"; shift 2 ;;
        --device=*)   DEVICE="${1#*=}"; shift ;;
        --weston)     USE_WESTON=yes; shift ;;
        --image)      CUSTOM_IMAGE="$2"; shift 2 ;;
        --image=*)    CUSTOM_IMAGE="${1#*=}"; shift ;;
        --list)       LIST_ONLY=yes; shift ;;
        --no-eject)   DO_EJECT=no; shift ;;
        -h|--help)    usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

# ── Locate image ──────────────────────────────────────────────────────────────
if [[ -n "$CUSTOM_IMAGE" ]]; then
    IMAGE="$CUSTOM_IMAGE"
elif [[ "$USE_WESTON" == "yes" ]]; then
    IMAGE="${DEPLOY_DIR}/core-image-weston-sparrow-hawk.rootfs.wic.gz"
else
    IMAGE="${DEPLOY_DIR}/core-image-minimal-sparrow-hawk.rootfs.wic.gz"
fi

# ── List mode ─────────────────────────────────────────────────────────────────
if [[ "$LIST_ONLY" == "yes" ]]; then
    echo "Candidate removable/MMC block devices:"
    lsblk -d -o NAME,SIZE,MODEL,RM,TYPE | grep -v "^loop\|^sr" | head -20
    exit 0
fi

# ── Validate image ────────────────────────────────────────────────────────────
if [[ ! -f "$IMAGE" ]]; then
    die "Image not found: $IMAGE\n  Build first: cd $REPO_ROOT && ./build.sh${USE_WESTON:+ --weston}"
fi

BMAP="${IMAGE%.gz}.bmap"
info "Image : $IMAGE"
[[ -f "$BMAP" ]] && info "Bmap  : $BMAP"

# ── Pick device ───────────────────────────────────────────────────────────────
if [[ -z "$DEVICE" ]]; then
    echo ""
    info "Available block devices:"
    lsblk -d -o NAME,SIZE,MODEL,RM | grep -v "^loop\|^sr"
    echo ""
    read -rp "$(echo -e "\033[1;33mEnter target device (e.g. /dev/sdb): \033[0m")" DEVICE
fi

[[ -b "$DEVICE" ]] || die "$DEVICE is not a block device."

# Guard against accidentally wiping the system disk
if lsblk -no MOUNTPOINT "$DEVICE" 2>/dev/null | grep -q "^/$"; then
    die "$DEVICE contains the root filesystem — refusing to flash."
fi

# ── Unmount partitions ────────────────────────────────────────────────────────
info "Unmounting any partitions on $DEVICE ..."
for part in "${DEVICE}"?*; do
    [[ -b "$part" ]] || continue
    if grep -q "^$part " /proc/mounts 2>/dev/null; then
        sudo umount "$part" && info "  unmounted $part" || warn "  could not unmount $part"
    fi
done

# ── Flash ─────────────────────────────────────────────────────────────────────
info "Flashing to $DEVICE ..."
if command -v bmaptool &>/dev/null; then
    info "Using bmaptool..."
    if [[ -f "$BMAP" ]]; then
        sudo bmaptool copy --bmap "$BMAP" "$IMAGE" "$DEVICE"
    else
        sudo bmaptool copy "$IMAGE" "$DEVICE"
    fi
else
    warn "bmaptool not found — using dd (slower). Install with: sudo apt install bmap-tools"
    zcat "$IMAGE" | sudo dd of="$DEVICE" bs=4M status=progress conv=fsync
fi

sudo sync
ok "Sync complete."

# ── Eject ─────────────────────────────────────────────────────────────────────
if [[ "$DO_EJECT" == "yes" ]]; then
    sudo eject "$DEVICE" 2>/dev/null && ok "Card ejected safely." || warn "Eject failed — remove manually."
fi

echo ""
ok "flash-mmc complete. Insert card into Sparrow Hawk board and power on."
echo "  Serial console: picocom -b 921600 /dev/ttyUSB0"
