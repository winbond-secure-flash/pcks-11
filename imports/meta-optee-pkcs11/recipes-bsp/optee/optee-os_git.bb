DESCRIPTION = "OP-TEE OS"
LICENSE = "BSD-2-Clause & BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=c1f21c4f72f372ef38a5a4aee55ec173"

PV = "4.5.0+git${SRCPV}"
BRANCH = "master"
SRCREV = "0919de0f7c79ad35ad3c8ace5f823ad1344b4716"
SRC_URI = "git://github.com/OP-TEE/optee_os.git;branch=${BRANCH};protocol=https"

# CVE-2026-33317: OOB read/write in PKCS#11 TA get_attribute_value
SRC_URI += " \
    file://0010-ta-pkcs11-check-output-buffer-size-on-get-attribute-.patch \
    file://0011-ta-pkcs11-check-template-consistency-on-get-attribut.patch \
    file://0012-ta-pkcs11-fix-attribute-output-size-if-too-small-on-.patch \
"

# CVE-2026-41434: PKCS#11 unbounded recursion DoS (+ prerequisite bounds patch)
# CVE-2026-42546: mobj reference leak in cleanup_shm_refs()
SRC_URI += " \
    file://0013-ta-pkcs11-bound-attribute-reads-in-__trace_attribute.patch \
    file://0014-ta-pkcs-11-limit-indirect-attribute-recursion-depth.patch \
    file://0015-core-fix-cleanup_shm_refs-attribute-masking.patch \
"

S = "${WORKDIR}/git"

PACKAGE_ARCH = "${MACHINE_ARCH}"
DEPENDS = "python3-pyelftools-native python3-cryptography-native"
inherit python3native

PATCHTOOL = "git"

export CROSS_COMPILE64 = "${TARGET_PREFIX}"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# PLATFORM and PLATFORM_FLAVOR must be set by the integrating BSP layer
# (e.g. via a .bbappend or machine config).
PLATFORM ?= ""
PLATFORM_FLAVOR ?= ""

EXTRA_OEMAKE = " \
    PLATFORM=${PLATFORM} PLATFORM_FLAVOR=${PLATFORM_FLAVOR} \
    CFG_ARM64_core=y CFG_REE_FS=y CFG_RPMB_FS=n \
    CFG_PKCS11_TA=y \
    CFG_CORE_HEAP_SIZE=0x40000 \
    CROSS_COMPILE64=${TARGET_PREFIX} \
    CRYPTOGRAPHY_OPENSSL_NO_LEGACY=1 \
"

# Suppress standalone-app CFLAGS/LDFLAGS so OP-TEE Makefiles stay in control
CFLAGS:prepend = "--sysroot=${STAGING_DIR_HOST} "
LD[unexport] = "1"
LDFLAGS[unexport] = "1"
export CCcore = "${CC}"
export LDcore = "${LD}"
libdir[unexport] = "1"

do_compile() {
    oe_runmake
}

do_install() {
    # Install TA devkit headers
    install -d ${D}/usr/include/optee/export-user_ta/
    for f in ${B}/out/arm-plat-${PLATFORM}/export-ta_arm64/*; do
        cp -aR $f ${D}/usr/include/optee/export-user_ta/
    done

    # Install firmware images
    install -d ${D}/boot
    install -m 0644 ${S}/out/arm-plat-${PLATFORM}/core/tee.elf     ${D}/boot/tee-${MACHINE}.elf
    install -m 0644 ${S}/out/arm-plat-${PLATFORM}/core/tee-raw.bin ${D}/boot/tee-${MACHINE}.bin

    # Install PKCS#11 TA (UUID fd02c9da-…)
    install -d ${D}${nonarch_libdir}/optee_armtz
    install -m 0444 \
        ${S}/out/arm-plat-${PLATFORM}/ta/pkcs11/fd02c9da-306c-48c7-a49c-bbd827ae86ee.ta \
        ${D}${nonarch_libdir}/optee_armtz/
}

PACKAGES =+ "${PN}-pkcs11"
FILES:${PN}-pkcs11 = "${nonarch_libdir}/optee_armtz/fd02c9da-306c-48c7-a49c-bbd827ae86ee.ta"
RDEPENDS:${PN}-pkcs11 = "optee-client"

FILES:${PN} = "/boot"
FILES:${PN}-dev = "/usr/include/optee"
INSANE_SKIP:${PN}-dev = "staticdev"
SYSROOT_DIRS += "/boot"
