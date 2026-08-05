FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# W77Q RPC-IF initialisation patch for BL31
SRC_URI:append = " \
    file://0001-rcar-v4h-bl31-init-rpcif-w77q.patch \
"

# OP-TEE secure payload dispatcher
DEPENDS += "optee-os"
OPTEE_BIN = "${RECIPE_SYSROOT}/boot/tee-${MACHINE}.bin"

# Override do_ipl_compile to build with SPD=opteed instead of SPD=none
do_ipl_compile () {
    oe_runmake distclean
    oe_runmake ${CLEAN_OPT} PLAT=${PLATFORM} SPD=opteed BL32=${OPTEE_BIN} MBEDTLS_COMMON_MK=1 ${ATFW_OPT}
    oe_runmake ${BUILD_OPT} PLAT=${PLATFORM} SPD=opteed BL32=${OPTEE_BIN} MBEDTLS_COMMON_MK=1 ${ATFW_OPT}

    # Create ${S}/release folder to store output for compile tasks
    install -d ${S}/release

    # Move to ${S}/release and rename
    install ${S}/build/${PLATFORM}/release/bl31/bl31.elf                ${S}/release/bl31-${MACHINE}${ATFW_CONF}.elf
    install ${S}/build/${PLATFORM}/release/bl31.bin                     ${S}/release/bl31-${MACHINE}${ATFW_CONF}.bin
    install ${S}/build/${PLATFORM}/release/bl31.srec                    ${S}/release/bl31-${MACHINE}${ATFW_CONF}.srec
}
