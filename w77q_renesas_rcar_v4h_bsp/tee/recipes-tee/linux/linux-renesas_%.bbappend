FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

# Remove display overlays that are not available in this BSP
KERNEL_DEVICETREE:remove:sparrow-hawk = " \
    renesas/r8a779g3-sparrow-hawk-dsi-waveshare-panel.dtbo \
    renesas/r8a779g3-sparrow-hawk-rpi-display-2-5in.dtbo \
    renesas/r8a779g3-sparrow-hawk-rpi-display-2-7in.dtbo \
"

# OP-TEE device tree overlays and kernel config
SRC_URI:append:sparrow-hawk = " \
    file://sparrow-hawk-optee.dtsi;subdir=git/arch/arm64/boot/dts/renesas/ \
    file://sparrow-hawk-rpc-disabled.dtsi;subdir=git/arch/arm64/boot/dts/renesas/ \
    file://sparrow-hawk-spi-optee.cfg \
"

do_compile:prepend:sparrow-hawk () {
    echo '#include "sparrow-hawk-optee.dtsi"' >>  ${S}/arch/arm64/boot/dts/renesas/r8a779g3-sparrow-hawk.dts
    echo '#include "sparrow-hawk-rpc-disabled.dtsi"' >>  ${S}/arch/arm64/boot/dts/renesas/r8a779g3-sparrow-hawk.dts
}
