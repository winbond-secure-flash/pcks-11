SUMMARY = "W77Q flash section key provisioning tool"
DESCRIPTION = "Userspace tool to provision a 128-bit password into a \
Winbond W77Q secure flash section via the OP-TEE provisioning Pseudo-TA."
HOMEPAGE = "https://github.com/renesas-rcar/meta-sparrow-hawk"
SECTION = "security"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause;md5=cb641bc04cda31daea161b1bc15da69f"

DEPENDS = "optee-client"

SRC_URI = "file://w77q-provision.c"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

S = "${WORKDIR}"

COMPATIBLE_MACHINE = "sparrow-hawk"
PACKAGE_ARCH = "${MACHINE_ARCH}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} \
        -I${STAGING_INCDIR} \
        -o ${B}/w77q-provision \
        ${WORKDIR}/w77q-provision.c \
        -lteec
}

do_install() {
    install -d ${D}${sbindir}
    install -m 0755 ${B}/w77q-provision ${D}${sbindir}/w77q-provision
}

FILES:${PN} = "${sbindir}/w77q-provision"
RDEPENDS:${PN} = "optee-client"
