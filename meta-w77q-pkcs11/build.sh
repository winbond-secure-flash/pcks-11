#!/bin/bash
#
# Build script for meta-w77q-pkcs11 (W77Q secure flash + PKCS#11 layer)
#
# Orchestrates all 4 repos:
#   1) meta-sparrow-hawk       — upstream BSP
#   2) w77q_renesas_rcar_v4h_bsp — OP-TEE HW enablement + CI
#   3) meta-optee-pkcs11      — upstream OP-TEE OS + PKCS#11 TA
#   4) meta-w77q-pkcs11       — W77Q/QLIB secure storage (this repo)
#
# Wraps the upstream meta-sparrow-hawk build.sh, injecting all layers.
#

SCRIPT_DIR=$(cd $(dirname $0) && pwd)
META_W77Q="${SCRIPT_DIR}"

# Locate sibling repos
SPARROW_HAWK="${SCRIPT_DIR}/../imports/meta-sparrow-hawk"
BSP_EXT="${SCRIPT_DIR}/../w77q_renesas_rcar_v4h_bsp"
OPTEE_PKCS11="${SCRIPT_DIR}/../imports/meta-optee-pkcs11"

if [ ! -d "$SPARROW_HAWK" ]; then
    echo "ERROR: Could not find meta-sparrow-hawk at $SPARROW_HAWK"
    echo "Expected location: imports/meta-sparrow-hawk (relative to project root)"
    exit 1
fi

if [ ! -d "$BSP_EXT" ]; then
    echo "ERROR: Could not find w77q_renesas_rcar_v4h_bsp at $BSP_EXT"
    exit 1
fi

if [ ! -d "$OPTEE_PKCS11" ]; then
    echo "ERROR: Could not find meta-optee-pkcs11 at $OPTEE_PKCS11"
    echo "Expected location: imports/meta-optee-pkcs11 (relative to project root)"
    exit 1
fi

MACHINE=sparrow-hawk
WORK="${SPARROW_HAWK}/build"

# Yocto requires a native Linux filesystem (symlinks, case sensitivity).
# When running under WSL on /mnt/*, redirect the build directory to a native path.
case "$SPARROW_HAWK" in
    /mnt/*)
        WORK="$HOME/yocto-sparrow-hawk"
        echo "Detected WSL mount — redirecting build to $WORK"
        mkdir -p "$WORK"
        rm -rf "$SPARROW_HAWK/build"
        ln -sf "$WORK" "$SPARROW_HAWK/build"
        ;;
esac

# Determine template variant
TEMPLATE_POSTFIX=""
for arg in "$@"; do
    if [[ "$arg" == "--weston" ]]; then
        TEMPLATE_POSTFIX="-weston"
    fi
done

# Ensure meta-w77q template has all required files (copy missing ones from upstream)
UPSTREAM_TEMPLATE="${SPARROW_HAWK}/conf/templates/${MACHINE}${TEMPLATE_POSTFIX}"
W77Q_TEMPLATE="${META_W77Q}/conf/templates/${MACHINE}${TEMPLATE_POSTFIX}"
if [ -d "$W77Q_TEMPLATE" ] && [ -d "$UPSTREAM_TEMPLATE" ]; then
    for f in "$UPSTREAM_TEMPLATE"/*; do
        base=$(basename "$f")
        if [ ! -e "$W77Q_TEMPLATE/$base" ]; then
            cp "$f" "$W77Q_TEMPLATE/$base"
        fi
    done
fi

# Create symlinks for all layers in the build area
mkdir -p "$WORK"
ln -snf "$(cd "$SPARROW_HAWK" && pwd)" "$WORK/meta-sparrow-hawk"
ln -snf "$META_W77Q" "$WORK/meta-w77q"
# BSP layer: in dev repo it's under tee/, in release it's at top level
if [ -d "$BSP_EXT/tee/conf" ]; then
    ln -snf "$(cd "$BSP_EXT" && pwd)/tee" "$WORK/w77q-renesas-rcar-v4h-bsp"
else
    ln -snf "$(cd "$BSP_EXT" && pwd)" "$WORK/w77q-renesas-rcar-v4h-bsp"
fi
ln -snf "$(cd "$OPTEE_PKCS11" && pwd)" "$WORK/meta-optee-pkcs11"

# Ensure Poky and meta-openembedded are present (clone if missing)
if [ ! -d "$WORK/poky" ]; then
    echo "Cloning Poky (Scarthgap)..."
    git clone --branch scarthgap --depth 1 https://git.yoctoproject.org/poky "$WORK/poky"
fi
if [ ! -d "$WORK/meta-openembedded" ]; then
    echo "Cloning meta-openembedded (Scarthgap)..."
    git clone --branch scarthgap --depth 1 https://git.openembedded.org/meta-openembedded "$WORK/meta-openembedded"
fi

# Patch upstream build.sh on the fly:
#  - Replace TEMPLATECONF to use meta-w77q's template (includes all layers)
#  - Remove top-level git clone lines (already handled above)
cd "$SPARROW_HAWK"
if [ -d "$W77Q_TEMPLATE" ]; then
    sed -e "s|TEMPLATECONF=\${WORK}/meta-sparrow-hawk/conf/templates/\$MACHINE\${TEMPLATE_POSTFIX}|TEMPLATECONF=\${WORK}/meta-w77q/conf/templates/\$MACHINE\${TEMPLATE_POSTFIX}|" \
        -e '/^git clone /d' \
        build.sh | bash -s -- "$@"
else
    sed -e '/^git clone /d' \
        build.sh | bash -s -- "$@"
    # Fallback: add all layers to bblayers.conf
    BBLAYERS_CONF="${WORK}/build-${MACHINE}/conf/bblayers.conf"
    if [ -f "$BBLAYERS_CONF" ]; then
        if ! grep -q "w77q-renesas-rcar-v4h-bsp" "$BBLAYERS_CONF"; then
            sed -i "/meta-sparrow-hawk/a\\  \${TOPDIR}/../w77q-renesas-rcar-v4h-bsp \\\\" "$BBLAYERS_CONF"
        fi
        if ! grep -q "meta-optee-pkcs11" "$BBLAYERS_CONF"; then
            sed -i "/w77q-renesas-rcar-v4h-bsp/a\\  \${TOPDIR}/../meta-optee-pkcs11 \\\\" "$BBLAYERS_CONF"
        fi
        if ! grep -q "meta-w77q" "$BBLAYERS_CONF"; then
            sed -i "/meta-optee-pkcs11/a\\  \${TOPDIR}/../meta-w77q \\\\" "$BBLAYERS_CONF"
            echo "Added all layers to bblayers.conf"
        fi
    fi
fi

# ---------- Copy outputs ----------
DEPLOY_SRC="${WORK}/build-${MACHINE}/tmp/deploy/images/${MACHINE}"
OUTPUT_DIR="${META_W77Q}/output_pkcs11"
if [ -d "$DEPLOY_SRC" ]; then
    rm -rf "${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}"
    echo "==> Copying build artifacts to ${OUTPUT_DIR}/"
    cp -f "${DEPLOY_SRC}/core-image-minimal-${MACHINE}.rootfs.wic.gz"  "${OUTPUT_DIR}/" 2>/dev/null || true
    cp -f "${DEPLOY_SRC}/core-image-minimal-${MACHINE}.rootfs.wic.bmap" "${OUTPUT_DIR}/" 2>/dev/null || true
    cp -f "${DEPLOY_SRC}/flash.bin"                                     "${OUTPUT_DIR}/" 2>/dev/null || true
    cp -f "${DEPLOY_SRC}/fitImage"                                      "${OUTPUT_DIR}/" 2>/dev/null || true
    echo ""
    echo "==> PKCS#11 build complete. Artifacts in: ${OUTPUT_DIR}/"
    ls -lh "${OUTPUT_DIR}/"
fi
