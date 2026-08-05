FILESEXTRAPATHS:prepend := "${THISDIR}/../../tests/pkcs11:"

DESCRIPTION = "OP-TEE PKCS#11 and secure storage on-target utilities"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

PACKAGE_ARCH = "${MACHINE_ARCH}"
COMPATIBLE_MACHINE = "sparrow-hawk"

SRC_URI = " \
    file://check-pkcs11.sh \
    file://check-tee-storage.sh \
    file://pkcs-test-menu.sh \
    file://pkcs-test-menu.py \
    file://pkcs11-rsa-sign-recover.sh \
    file://verify-install.sh \
"

RDEPENDS:${PN} = "optee-client optee-os-pkcs11 libckteec opensc"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/check-pkcs11.sh              ${D}${bindir}/check-pkcs11.sh
    install -m 0755 ${WORKDIR}/check-tee-storage.sh         ${D}${bindir}/check-tee-storage.sh
    install -m 0755 ${WORKDIR}/pkcs-test-menu.sh            ${D}${bindir}/pkcs-test-menu.sh
    install -m 0755 ${WORKDIR}/pkcs11-rsa-sign-recover.sh   ${D}${bindir}/pkcs11-rsa-sign-recover.sh
    install -m 0755 ${WORKDIR}/verify-install.sh             ${D}${bindir}/verify-install.sh
    install -m 0755 ${WORKDIR}/pkcs-test-menu.py             ${D}${bindir}/pkcs-test-menu.py
}

FILES:${PN} = " \
    ${bindir}/check-pkcs11.sh \
    ${bindir}/check-tee-storage.sh \
    ${bindir}/pkcs-test-menu.sh \
    ${bindir}/pkcs-test-menu.py \
    ${bindir}/pkcs11-rsa-sign-recover.sh \
    ${bindir}/verify-install.sh \
"
