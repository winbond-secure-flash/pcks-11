DESCRIPTION = "OP-TEE sample Trusted Applications and their client applications"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=cd95ab417e23b94f381dafc453d70c30"

PACKAGE_ARCH = "${MACHINE_ARCH}"

DEPENDS = "optee-os optee-client python3-pyelftools-native python3-cryptography-native"
inherit python3native

PV = "4.5.0+git${SRCPV}"
BRANCH = "master"
SRCREV = "934c7edb74a26e90f68024cf441073528444177f"
SRC_URI = "git://github.com/linaro-swg/optee_examples.git;branch=${BRANCH};protocol=https"

S = "${WORKDIR}/git"

TA_DEV_KIT_DIR = "${STAGING_INCDIR}/optee/export-user_ta"
TEEC_EXPORT = "${STAGING_DIR_HOST}/usr"

export CROSS_COMPILE64 = "${TARGET_PREFIX}"

# Let OP-TEE Makefiles manage their own flags
CFLAGS[unexport] = "1"
AS[unexport] = "1"
LD[unexport] = "1"

EXTRA_OEMAKE = " \
    CROSS_COMPILE=${TARGET_PREFIX} \
    TA_DEV_KIT_DIR=${TA_DEV_KIT_DIR} \
    TEEC_EXPORT=${TEEC_EXPORT} \
    CRYPTOGRAPHY_OPENSSL_NO_LEGACY=1 \
"

do_compile() {
    # Yocto's gcc wrapper returns just "libgcc.a" for -print-libgcc-file-name when
    # the sysroot is separated from the toolchain; resolve the real path and pass
    # it directly as the ta_arm64 libgcc override expected by the TA devkit.
    libgcc=$(find ${STAGING_DIR_HOST} -name "libgcc.a" -path "*aarch64-poky-linux*" | head -1)
    oe_runmake examples libgccta_arm64="${libgcc}"
}

do_install() {
    # Install TA binaries (UUID-named .ta files) for tee-supplicant to load
    install -d ${D}${nonarch_libdir}/optee_armtz
    find ${S} -name "*.ta" -exec install -m 0444 {} ${D}${nonarch_libdir}/optee_armtz/ \;

    # Install host CA binaries
    install -d ${D}/${bindir}
    find ${S} -path "*/host/optee_example_*" -not -name "*.o" -type f \
        -exec install -m 0755 {} ${D}/${bindir}/ \;

    # The plugins example builds tee-supplicant plugins in-tree under plugins/*.
    install -d ${D}${libdir}/tee-supplicant/plugins
    find ${S}/plugins -name "*.plugin" -type f \
        -exec install -m 0444 {} ${D}${libdir}/tee-supplicant/plugins/ \;
}

FILES:${PN} = "${bindir} ${nonarch_libdir}/optee_armtz ${libdir}/tee-supplicant/plugins"
