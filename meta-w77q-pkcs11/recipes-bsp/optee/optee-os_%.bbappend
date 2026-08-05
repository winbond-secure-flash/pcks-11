FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Board-specific OP-TEE platform settings (sparrow-hawk / R-Car V4H)
PLATFORM = "rcar"
PLATFORM_FLAVOR = "spider_s4"

# Override storage backend: disable REE-FS, enable W77Q secure flash
EXTRA_OEMAKE:remove = "CFG_REE_FS=y"
EXTRA_OEMAKE += " \
    CFG_REE_FS=n \
    CFG_W77Q=y CFG_W77Q_FS=y CFG_W77Q_QLIB=y \
    CFG_W77Q_DUMP_PTA=y \
"

# Fetch QLIB artifacts via recipe dependency instead of from an in-tree folder.
DEPENDS:append = " qlib-bin"

# W77Q patches and integration sources
SRC_URI:append = " \
    file://0001-rcar-rpcif-add-spi-driver.patch \
    file://0002-rcar-w77q-flash-driver.patch \
    file://0003-tee-fs-w77q-storage-backend.patch \
    file://0004-rcar-rpcif-direct-mode-reads.patch \
    file://0006-qlib-in-tee-wire-build.patch \
    file://0007-w77q-provision-pta.patch \
    file://0008-w77q-dump-pta.patch \
    file://0009-pkcs11-ta-remove-instance-keep-alive.patch \
    file://qlib-tee/sub.mk;subdir=qlib-tee \
    file://qlib-tee/w77q_qlib.c;subdir=qlib-tee \
    file://qlib-tee/w77q_qlib_provision.c;subdir=qlib-tee \
    file://qlib-tee/qlib_platform.c;subdir=qlib-tee \
    file://qlib-tee/w77q_config.h;subdir=qlib-tee \
    file://layer2_storage/tee_lfs_storage.c;subdir=layer2_storage \
    file://layer2_storage/tee_lfs_storage.h;subdir=layer2_storage \
    file://layer2_storage/lfs_w77q_cfg.h;subdir=layer2_storage \
    file://layer2_storage/w77q_lfs_port.c;subdir=layer2_storage \
    file://layer2_storage/w77q_lfs_port.h;subdir=layer2_storage \
    git://github.com/littlefs-project/littlefs.git;protocol=https;nobranch=1;destsuffix=littlefs;name=littlefs \
"

# LittleFS v2.11.3 — pinned commit for reproducible builds
SRCREV_littlefs = "6cb4e86540eca0d9ba62500a298385c9d863c8be"

# Required when multiple SCMs are used — tells BitBake how to form the composite hash
SRCREV_FORMAT = "default_littlefs"

# Install the prebuilt QLIB static library and public headers plus the
# integration files into the OP-TEE source tree before the build starts.
# The qlib/ subdirectory sits alongside rcar_rpcif.c in core/drivers/spi/
# and is wired in by sub.mk when CFG_W77Q_QLIB=y.  No QLIB C sources are
# compiled — libqlib.a is linked into the Secure World core instead.
do_configure:prepend() {
    QLIB_DST="${S}/core/drivers/spi/qlib"
    QLIB_INC="${RECIPE_SYSROOT}${includedir}/qlib"
    QLIB_LIB="${RECIPE_SYSROOT}${libdir}/libqlib.a"

    if [ ! -f "${QLIB_LIB}" ]; then
        bbfatal "QLIB static library not found at ${QLIB_LIB}. Ensure qlib-bin was built and staged."
    fi
    if [ ! -d "${QLIB_INC}" ]; then
        bbfatal "QLIB headers not found at ${QLIB_INC}. Ensure qlib-bin was built and staged."
    fi

    install -d ${QLIB_DST}/inc
    install -d ${QLIB_DST}/defs
    install -d ${QLIB_DST}/lib

    # Public API headers (qlib.h, qlib_platform.h, qlib_types.h, ...)
    cp ${QLIB_INC}/*.h       ${QLIB_DST}/inc/

    # Winbond portability/defs layer headers
    cp ${QLIB_INC}/defs/*.h  ${QLIB_DST}/defs/

    # Prebuilt aarch64 static library — linked into the OP-TEE core
    install -m 644 ${QLIB_LIB} ${QLIB_DST}/lib/

    # Integration files: sub.mk, w77q_qlib.c, platform callbacks
    INTEG="${WORKDIR}/qlib-tee/qlib-tee"
    install -m 644 ${INTEG}/sub.mk                       ${QLIB_DST}/
    install -m 644 ${INTEG}/w77q_qlib.c                  ${QLIB_DST}/
    install -m 644 ${INTEG}/w77q_qlib_provision.c        ${QLIB_DST}/
    install -m 644 ${INTEG}/qlib_platform.c              ${QLIB_DST}/
    install -m 644 ${INTEG}/w77q_config.h                 ${QLIB_DST}/

    # W77Q FS layer-2 storage backend sources (LittleFS-based).
    # Installed to core/tee/ before patches are applied so that the
    # sub.mk entry added by 0003-tee-fs-w77q-storage-backend.patch can
    # compile them.
    L2S="${WORKDIR}/layer2_storage/layer2_storage"
    install -m 644 ${L2S}/tee_lfs_storage.c  ${S}/core/tee/
    install -m 644 ${L2S}/tee_lfs_storage.h  ${S}/core/tee/
    install -m 644 ${L2S}/lfs_w77q_cfg.h     ${S}/core/tee/
    install -m 644 ${L2S}/w77q_lfs_port.c    ${S}/core/tee/
    install -m 644 ${L2S}/w77q_lfs_port.h    ${S}/core/tee/

    # LittleFS upstream source — fetched from git, pinned to v2.11.3
    LFS_SRC="${WORKDIR}/littlefs"
    install -m 644 ${LFS_SRC}/lfs.c              ${S}/core/tee/
    install -m 644 ${LFS_SRC}/lfs.h              ${S}/core/tee/
    install -m 644 ${LFS_SRC}/lfs_util.c         ${S}/core/tee/
    install -m 644 ${LFS_SRC}/lfs_util.h         ${S}/core/tee/
    install -m 644 ${INTEG}/w77q_config.h    ${S}/core/tee/
}
