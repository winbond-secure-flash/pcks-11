DESCRIPTION = "Secure Counter — minimal custom OP-TEE TA + CA example for Sparrow Hawk"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

PACKAGE_ARCH = "${MACHINE_ARCH}"
COMPATIBLE_MACHINE = "sparrow-hawk"

DEPENDS = "optee-os optee-client python3-pyelftools-native python3-cryptography-native"
inherit python3native

SRC_URI = " \
    file://ta/secure_counter_ta.c \
    file://ta/secure_counter_ta.h \
    file://ta/user_ta_header_defines.h \
    file://ta/sub.mk \
    file://ta/Makefile \
    file://host/main.c \
"

S = "${WORKDIR}"

TA_DEV_KIT_DIR = "${STAGING_INCDIR}/optee/export-user_ta"
TEEC_EXPORT     = "${STAGING_DIR_HOST}/usr"
TA_UUID         = "6e256cba-fc4d-4c2b-a7f2-6b5c4d9e7a8b"

export CROSS_COMPILE64 = "${TARGET_PREFIX}"

# Let OP-TEE Makefiles manage their own flags (same fix as optee-examples)
CFLAGS[unexport] = "1"
AS[unexport]     = "1"
LD[unexport]     = "1"

do_compile() {
    # ── Build the Trusted Application (Secure World) ──────────────────────
    # Resolve libgcc.a — Yocto's gcc wrapper returns bare "libgcc.a" for
    # -print-libgcc-file-name; find the real path in the sysroot.
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
        -o ${WORKDIR}/secure-counter \
        -lteec
}

do_install() {
    # Install signed TA → tee-supplicant loads it from here at runtime
    install -d ${D}${nonarch_libdir}/optee_armtz
    install -m 0444 \
        ${WORKDIR}/ta/${TA_UUID}.ta \
        ${D}${nonarch_libdir}/optee_armtz/

    # Install CA binary
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/secure-counter ${D}${bindir}/secure-counter
}

RDEPENDS:${PN} = "optee-client"

FILES:${PN} = "${bindir}/secure-counter ${nonarch_libdir}/optee_armtz/${TA_UUID}.ta"
