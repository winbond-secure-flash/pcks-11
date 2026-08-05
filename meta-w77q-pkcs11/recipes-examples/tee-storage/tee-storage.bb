DESCRIPTION = "TEE Storage — userspace read/write/erase/format utility for \
OP-TEE secure storage (W77Q SPI flash backend) on Sparrow Hawk. \
Provides named-object and sector-addressed access from a single CLI."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "optee-client optee-os python3-cryptography-native python3-pyelftools-native"
inherit python3native

PACKAGE_ARCH = "${MACHINE_ARCH}"
COMPATIBLE_MACHINE = "sparrow-hawk"

SRC_URI = " \
    file://ta/tee_storage_ta.c \
    file://ta/tee_storage_ta.h \
    file://ta/user_ta_header_defines.h \
    file://ta/sub.mk \
    file://ta/Makefile \
    file://host/main.c \
    file://host/w77q_dump.c \
    file://host/w77q_dump_ta.h \
"

S = "${WORKDIR}"

TA_DEV_KIT_DIR = "${STAGING_INCDIR}/optee/export-user_ta"
TEEC_EXPORT    = "${STAGING_DIR_HOST}/usr"
TA_UUID        = "a6f6e048-f6c4-4a0e-b629-4b4d9e8c1d5e"

export CROSS_COMPILE64 = "${TARGET_PREFIX}"

CFLAGS[unexport] = "1"
AS[unexport]     = "1"
LD[unexport]     = "1"

do_compile() {
    # ── Build the Trusted Application (Secure World) ──────────────────────
    libgcc=$(find ${STAGING_DIR_HOST} -name "libgcc.a" \
                  -path "*aarch64-poky-linux*" | head -1)

    oe_runmake -C ${WORKDIR}/ta \
        CROSS_COMPILE=${TARGET_PREFIX} \
        TA_DEV_KIT_DIR=${TA_DEV_KIT_DIR} \
        CRYPTOGRAPHY_OPENSSL_NO_LEGACY=1 \
        libgccta_arm64="${libgcc}"

    # ── Build the Client Application (Normal World) ───────────────────────
    ${CC} ${CFLAGS} ${LDFLAGS} \
        -I${WORKDIR}/ta \
        -I${TEEC_EXPORT}/include \
        -L${TEEC_EXPORT}/lib \
        ${WORKDIR}/host/main.c \
        -o ${WORKDIR}/tee-storage \
        -lteec

    # ── Build w77q-dump (w77q_fs LUT dump tool via Pseudo-TA) ─────────────
    ${CC} ${CFLAGS} ${LDFLAGS} \
        -I${WORKDIR}/host \
        -I${TEEC_EXPORT}/include \
        -L${TEEC_EXPORT}/lib \
        ${WORKDIR}/host/w77q_dump.c \
        -o ${WORKDIR}/w77q-dump \
        -lteec
}

do_install() {
    # Install signed TA — tee-supplicant loads it from here at runtime
    install -d ${D}${nonarch_libdir}/optee_armtz
    install -m 0444 \
        ${WORKDIR}/ta/${TA_UUID}.ta \
        ${D}${nonarch_libdir}/optee_armtz/

    # Install CLI binary
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/tee-storage ${D}${bindir}/tee-storage
    install -m 0755 ${WORKDIR}/w77q-dump   ${D}${bindir}/w77q-dump
}

RDEPENDS:${PN} = "optee-client"

FILES:${PN} = "${bindir}/tee-storage ${bindir}/w77q-dump ${nonarch_libdir}/optee_armtz/${TA_UUID}.ta"
