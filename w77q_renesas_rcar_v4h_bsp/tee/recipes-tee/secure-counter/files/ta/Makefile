# TA Makefile — consumed by the OP-TEE TA devkit
#
# BINARY must be the TA UUID (without .ta extension).
# The devkit produces:  $(BINARY).ta  (signed, ready to load)
#
# Build:
#   make TA_DEV_KIT_DIR=<path/to/export-user_ta> CROSS_COMPILE64=aarch64-...-

BINARY = 6e256cba-fc4d-4c2b-a7f2-6b5c4d9e7a8b

ifneq ($(wildcard $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk),)
include $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk
else
$(error TA_DEV_KIT_DIR is not set or does not point to the OP-TEE TA devkit)
endif
