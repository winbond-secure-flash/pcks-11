DESCRIPTION = "OP-TEE Client"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=69663ab153298557a59c67a60a743e5b"

require include/optee-common.inc
require include/optee-${MACHINE}.inc

SRC_URI:pn-optee-client += "file://optee.service file://tee-supplicant"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

DEPENDS += "util-linux"

PACKAGE_ARCH = "${MACHINE_ARCH}"
inherit python3native systemd update-rc.d pkgconfig

INITSCRIPT_NAME = "tee-supplicant"
INITSCRIPT_PARAMS = "defaults 90"

SYSTEMD_SERVICE:${PN} = "optee.service"

S = "${WORKDIR}/git"

EXTRA_OEMAKE = "RPMB_EMU=0 PKG_CONFIG=pkg-config"

do_install() {
    install -d ${D}/${libdir}
    install -d ${D}/${includedir}

    install -m 0755 ${S}/out/export/usr/lib/libteec.so.2.0 ${D}/${libdir}
    cd ${D}/${libdir}
    ln -sf libteec.so.2.0 libteec.so.2
    ln -sf libteec.so.2 libteec.so

    # PKCS#11 client library — install all versioned files and symlinks
    # (build export provides: .so.0.1.0, .so.0.1, .so.0, .so)
    for f in ${S}/out/export/usr/lib/libckteec.so*; do
        if [ -L "$f" ]; then
            ln -sf "$(readlink $f)" ${D}/${libdir}/$(basename $f)
        elif [ -f "$f" ]; then
            install -m 0755 "$f" ${D}/${libdir}/
        fi
    done

    install -m 0644 ${S}/out/export/usr/include/* ${D}/${includedir}

    if ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'true', 'false', d)}; then
        install -d ${D}/${systemd_system_unitdir}
        install -m 0644 ${WORKDIR}/optee.service ${D}/${systemd_system_unitdir}
    fi
    # Always install sysvinit script — used when systemd is in DISTRO_FEATURES
    # but the actual init manager is sysvinit (e.g. core-image-minimal).
    install -d ${D}/${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/tee-supplicant ${D}/${sysconfdir}/init.d/tee-supplicant
}

# tee-supplicant is 64-bit only
do_install:append:aarch64() {
    install -d ${D}/${bindir}
    install -m 0755 ${S}/out/export/usr/sbin/tee-supplicant ${D}/${bindir}
}

PACKAGES =+ "libckteec"
FILES:libckteec = "${libdir}/libckteec.so.* ${libdir}/libckteec.so.0"
FILES:libckteec-dev = "${libdir}/libckteec.so"
RPROVIDES:${PN} += "optee-client"
FILES:${PN} += "${libdir} ${includedir}"
