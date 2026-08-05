# TA Makefile — consumed by the OP-TEE TA devkit
#
# BINARY must be the TA UUID (without .ta extension).
# The devkit produces:  $(BINARY).ta  (signed ELF, loaded by tee-supplicant)
#
# Build:
#   make TA_DEV_KIT_DIR=<path/to/export-user_ta> CROSS_COMPILE64=aarch64-...-

BINARY = a6f6e048-f6c4-4a0e-b629-4b4d9e8c1d5e

ifneq ($(wildcard $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk),)
include $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk
else
$(error TA_DEV_KIT_DIR is not set or does not point to the OP-TEE TA devkit)
endif
