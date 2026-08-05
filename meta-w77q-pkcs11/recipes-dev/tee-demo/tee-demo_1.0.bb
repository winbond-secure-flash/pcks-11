# SPDX-License-Identifier: BSD-2-Clause
#
# tee-demo_1.0.bb — W77Q QLIB TEE demo: Trusted Application + host app.

FILESEXTRAPATHS:prepend := "${THISDIR}/../../tests/tee-demo:"
#
# Builds and installs:
#   ${nonarch_base_libdir}/optee_armtz/<UUID>.ta  (Trusted Application)
#   ${bindir}/tee-demo                            (Linux host application)
#
# Prerequisites: optee-os (for TA devkit) + optee-client (for libteec/headers)

SUMMARY = "W77Q QLIB secure storage demo: TA + Linux host app"
DESCRIPTION = "Demonstrates password-protected TEE secure storage backed by \
    the W77Q QLIB flash library running in OP-TEE Secure World."
HOMEPAGE = "https://github.com/renesas-rcar/meta-sparrow-hawk"
SECTION = "security"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause;md5=cb641bc04cda31daea161b1bc15da69f"

DEPENDS = "optee-client optee-os python3-cryptography-native python3-pyelftools-native"
RDEPENDS:${PN} = "optee-client"

S = "${WORKDIR}"

SRC_URI = " \
    file://ta/tee_demo_ta.c \
    file://ta/sub.mk \
    file://ta/Makefile \
    file://ta/user_ta_header_defines.h \
    file://ta/include/tee_demo_ta.h \
    file://host/tee-demo.c \
    file://host/Makefile \
"

COMPATIBLE_MACHINE = "sparrow-hawk"

# OP-TEE TA devkit export directory (installed by optee-os recipe)
TA_DEV_KIT_DIR = "${STAGING_INCDIR}/optee/export-user_ta"

# optee-client sysroot: libteec + tee_client_api.h
TEEC_EXPORT = "${STAGING_DIR_TARGET}/usr"

inherit python3native

do_compile() {
    # Resolve libgcc path using Yocto's CC (includes --sysroot).
    # OP-TEE gcc.mk uses CC_ta_arm64 = CROSS_COMPILE+gcc (no sysroot), so
    # it may return a bare "libgcc.a" filename; override it explicitly.
    LIBGCC=$(${CC} -print-libgcc-file-name)

    # Build Trusted Application
    oe_runmake -C "${S}/ta" \
        TA_DEV_KIT_DIR="${TA_DEV_KIT_DIR}" \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        CC="${CC}" \
        "libgccta_arm64=${LIBGCC}" \
        CRYPTOGRAPHY_OPENSSL_NO_LEGACY=1

    # Build host application
    oe_runmake -C "${S}/host" \
        TEEC_EXPORT="${TEEC_EXPORT}" \
        TA_INCLUDE="${S}/ta/include" \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        CC="${CC}" \
        "CFLAGS=-Wall -Wextra -O2 -I${TEEC_EXPORT}/include -I${S}/ta/include" \
        "LDFLAGS=${LDFLAGS} -L${TEEC_EXPORT}/lib -lteec"
}

do_install() {
    # Install Trusted Application binary to optee_armtz
    install -d "${D}${nonarch_base_libdir}/optee_armtz"
    install -m 0644 \
        "${S}/ta/b1c2d3e4-f5a6-7890-abcd-ef0123456789.ta" \
        "${D}${nonarch_base_libdir}/optee_armtz/"

    # Install host application
    install -d "${D}${bindir}"
    install -m 0755 "${S}/host/tee-demo" "${D}${bindir}/tee-demo"
}

FILES:${PN} += "${nonarch_base_libdir}/optee_armtz/b1c2d3e4-f5a6-7890-abcd-ef0123456789.ta \
    ${bindir}/tee-demo \
"
