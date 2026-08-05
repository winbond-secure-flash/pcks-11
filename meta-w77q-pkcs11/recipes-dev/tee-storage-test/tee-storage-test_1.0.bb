FILESEXTRAPATHS:prepend := "${THISDIR}/../../tests/tee-storage:"

SUMMARY = "Generic TEE secure storage test suite"
DESCRIPTION = "Trusted Application + Python host script that exercise every \
GP TEE secure storage code path (write, read, delete, exists, list, get_size, \
clear_all) via ctypes + libteec.so. 16 test cases cover round-trips, \
overwrite, empty values, binary data, 8 KB large objects, multi-key, \
persistence across sessions, and short-buffer error handling."
HOMEPAGE = "https://github.com/rcar-community/meta-sparrow-hawk"
SECTION = "security"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause;md5=cb641bc04cda31daea161b1bc15da69f"

DEPENDS = "optee-client optee-os python3-cryptography-native python3-pyelftools-native"
RDEPENDS:${PN} = "optee-client python3-core"

COMPATIBLE_MACHINE = "sparrow-hawk"
PACKAGE_ARCH = "${MACHINE_ARCH}"

S = "${WORKDIR}"

SRC_URI = " \
    file://ta/tee_storage_test_ta.c \
    file://ta/include/tee_storage_test_ta.h \
    file://ta/sub.mk \
    file://ta/Makefile \
    file://ta/user_ta_header_defines.h \
    file://tee_storage_test.py \
"

TA_DEV_KIT_DIR = "${STAGING_INCDIR}/optee/export-user_ta"

inherit python3native

do_compile() {
    LIBGCC=$(${CC} -print-libgcc-file-name)

    oe_runmake -C "${S}/ta" \
        TA_DEV_KIT_DIR="${TA_DEV_KIT_DIR}" \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        CC="${CC}" \
        "libgccta_arm64=${LIBGCC}" \
        CRYPTOGRAPHY_OPENSSL_NO_LEGACY=1
}

do_install() {
    install -d "${D}${nonarch_base_libdir}/optee_armtz"
    install -m 0444 \
        "${S}/ta/c3d4e5f6-a7b8-90ab-cdef-012345678901.ta" \
        "${D}${nonarch_base_libdir}/optee_armtz/"

    install -d "${D}${bindir}"
    install -m 0755 "${S}/tee_storage_test.py" "${D}${bindir}/tee_storage_test.py"
}

FILES:${PN} = " \
    ${nonarch_base_libdir}/optee_armtz/c3d4e5f6-a7b8-90ab-cdef-012345678901.ta \
    ${bindir}/tee_storage_test.py \
"
