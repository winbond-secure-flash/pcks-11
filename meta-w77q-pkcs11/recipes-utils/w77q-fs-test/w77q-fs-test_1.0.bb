FILESEXTRAPATHS:prepend := "${THISDIR}/../../tests/w77q-fs:"

SUMMARY = "w77q_fs secure storage filesystem tests"
DESCRIPTION = "Trusted Application + Linux host app that exercise every \
w77q_fs code path (create, read, update, rename, enumerate, large payload, \
overwrite, truncate) via the GP TEE Internal API. Each test case is \
self-contained: it creates its objects, tests the operation, cleans up, \
and returns PASS/FAIL."
HOMEPAGE = "https://github.com/renesas-rcar/meta-sparrow-hawk"
SECTION = "security"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause;md5=cb641bc04cda31daea161b1bc15da69f"

DEPENDS = "optee-client optee-os python3-cryptography-native python3-pyelftools-native"
RDEPENDS:${PN} = "optee-client"

COMPATIBLE_MACHINE = "sparrow-hawk"
PACKAGE_ARCH = "${MACHINE_ARCH}"

S = "${WORKDIR}"

SRC_URI = " \
    file://ta/w77q_fs_test_ta.c \
    file://ta/include/w77q_fs_test_ta.h \
    file://ta/sub.mk \
    file://ta/Makefile \
    file://ta/user_ta_header_defines.h \
    file://host/w77q_fs_test.c \
    file://host/Makefile \
"

TA_DEV_KIT_DIR = "${STAGING_INCDIR}/optee/export-user_ta"
TEEC_EXPORT     = "${STAGING_DIR_TARGET}/usr"

inherit python3native

do_compile() {
    LIBGCC=$(${CC} -print-libgcc-file-name)

    oe_runmake -C "${S}/ta" \
        TA_DEV_KIT_DIR="${TA_DEV_KIT_DIR}" \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        CC="${CC}" \
        "libgccta_arm64=${LIBGCC}" \
        CRYPTOGRAPHY_OPENSSL_NO_LEGACY=1

    oe_runmake -C "${S}/host" \
        TEEC_EXPORT="${TEEC_EXPORT}" \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        CC="${CC}" \
        "CFLAGS=-Wall -Wextra -O2 -I${TEEC_EXPORT}/include -I${S}/ta/include" \
        "LDFLAGS=${LDFLAGS} -L${TEEC_EXPORT}/lib -lteec"
}

do_install() {
    install -d "${D}${nonarch_base_libdir}/optee_armtz"
    install -m 0444 \
        "${S}/ta/f0e1d2c3-b4a5-9687-8796-a5b4c3d2e1f0.ta" \
        "${D}${nonarch_base_libdir}/optee_armtz/"

    install -d "${D}${bindir}"
    install -m 0755 "${S}/host/w77q-fs-test" "${D}${bindir}/w77q-fs-test"
}

FILES:${PN} = " \
    ${nonarch_base_libdir}/optee_armtz/f0e1d2c3-b4a5-9687-8796-a5b4c3d2e1f0.ta \
    ${bindir}/w77q-fs-test \
"
