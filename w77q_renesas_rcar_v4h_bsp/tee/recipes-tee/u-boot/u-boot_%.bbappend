FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

# Add OP-TEE FIT loadable handler so TF-A receives BL32 entry point
SRC_URI:append = " file://0002-renesas-gen4-add-optee-fit-loadable-handler.patch"

# W77Q needs clk_ignore_unused in bootargs
SRC_URI:append = " file://mmc_boot.cfg"

# Suppress patch-fuzz QA error (context offset is benign)
ERROR_QA:remove = "patch-fuzz"
WARN_QA:append = " patch-fuzz"
