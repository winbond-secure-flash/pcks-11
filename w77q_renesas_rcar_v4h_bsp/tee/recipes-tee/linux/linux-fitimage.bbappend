FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

# Add OP-TEE OS dependency
DEPENDS += "optee-os"

# Replace boot.cmd and fit-image.its with W77Q versions (includes OP-TEE node)
SRC_URI:append = " \
    file://boot.cmd \
    file://fit-image.its \
"

do_compile[depends] += "optee-os:do_populate_sysroot"

do_compile() {
    cd ${DEPLOY_DIR}/images/${MACHINE}
    install -m 644 ${WORKDIR}/boot.cmd ./
    install -m 644 ${WORKDIR}/fit-image.its ./
    install -m 644 ${RECIPE_SYSROOT}/boot/tee-${MACHINE}.bin ./tee.bin
    sed -i "s/bl31.bin/bl31-${MACHINE}.bin/" ./fit-image.its
    mkimage -f ./fit-image.its ./fitImage
}
