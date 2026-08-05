DESCRIPTION = "OP-TEE PKCS#11 demo application for Sparrow Hawk"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

PACKAGE_ARCH = "${MACHINE_ARCH}"
COMPATIBLE_MACHINE = "sparrow-hawk"

SRC_URI = "file://pkcs11-demo.c"

S = "${WORKDIR}"

# optee-client provides libckteec.so + pkcs11.h in the sysroot
DEPENDS = "optee-client"
RDEPENDS:${PN} = "optee-client libckteec opensc optee-os-pkcs11"

do_configure[noexec] = "1"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} \
        pkcs11-demo.c -o pkcs11-demo \
        -lckteec
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/pkcs11-demo ${D}${bindir}/pkcs11-demo
}

FILES:${PN} = "${bindir}/pkcs11-demo"
