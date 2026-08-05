# w77q_renesas_rcar_v4h_bsp

Yocto CI layer for the Renesas R-Car V4H Sparrow Hawk board. Adds auto-boot
support so the board can recover from power failures without manual intervention.

## What it does

| Feature | Mechanism |
|---------|-----------|
| Auto-login as root | systemd serial-getty drop-in (`--autologin root`) |
| Static IP (10.195.1.217/24 on end0) | systemd-networkd `.network` file |
| SPI flash bridge module auto-load | `kernel-module-spi-flash-bridge` recipe with `KERNEL_MODULE_AUTOLOAD` |

## Build

```bash
# Clone and build everything (fetches meta-sparrow-hawk from GitHub)
./build_CI.sh

# Use a local BSP copy instead of cloning
./build_CI.sh --bsp-path /path/to/meta-sparrow-hawk

# Save disk space
./build_CI.sh --rm-work
```

## Layer structure

```
conf/layer.conf                         — layer registration (priority 7, depends on sparrow-hawk)
conf/templates/sparrow-hawk-ci/         — TEMPLATECONF for the CI build
include/                                — shared OP-TEE version/platform includes
recipes-ci/ci-autoboot/                 — auto-login + static IP
recipes-ci/spi-flash-bridge/            — out-of-tree SPI flash bridge kernel module
recipes-core/images/                    — IMAGE_INSTALL additions
recipes-tee/arm-trusted-firmware/       — BL31 bbappend + W77Q RPC-IF init patch
recipes-tee/optee/                      — OP-TEE client library + tee-supplicant service
recipes-tee/u-boot/                     — U-Boot bbappend + OP-TEE FIT loadable handler
recipes-tee/linux/                      — kernel bbappends (FIT image, DT overlays, config)
recipes-tee/secure-counter/             — example OP-TEE trusted application + host client
recipes-tee/ipl-burning/                — IPL burning tool (W77Q-aware)
scripts/                                — deployment & flash utilities
output/                                 — pre-built fitImage + WIC bmap
```
