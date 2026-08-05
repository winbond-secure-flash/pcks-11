SUMMARY = "Prebuilt Winbond QLIB static library and headers"
DESCRIPTION = "Fetches qlib-bin from GitHub and exports libqlib.a and public headers to the Yocto sysroot."
HOMEPAGE = "https://github.com/winbond-secure-flash/qlib-bin"
LICENSE = "CLOSED"

SRC_URI = "git://github.com/winbond-secure-flash/qlib-bin.git;branch=main;protocol=https"
SRCREV = "3e8223cec5545a663f63d6a0f88934a080126452"

S = "${WORKDIR}/git"

# Keep recipe output deterministic when tracking a pinned git revision.
PV = "23.6+git${SRCPV}"

# QLIB is delivered as prebuilt binaries and headers only.
do_compile[noexec] = "1"

# Install headers and static archive so dependent recipes can consume them from
# RECIPE_SYSROOT during their configure/build stages.
do_install() {
    install -d ${D}${includedir}/qlib
    install -d ${D}${includedir}/qlib/defs
    install -d ${D}${libdir}

    cp ${S}/inc/*.h ${D}${includedir}/qlib/
    cp ${S}/inc/defs/*.h ${D}${includedir}/qlib/defs/

    if [ ! -f "${S}/bin/build-${TARGET_ARCH}/libqlib.a" ]; then
        bbfatal "Expected ${S}/bin/build-${TARGET_ARCH}/libqlib.a was not found."
    fi
    install -m 0644 ${S}/bin/build-${TARGET_ARCH}/libqlib.a ${D}${libdir}/
}

FILES:${PN}-dev += "${includedir}/qlib"
FILES:${PN}-staticdev += "${libdir}/libqlib.a"
