# SPDX-License-Identifier: BSD-2-Clause
#
# Makefile for the W77Q demo Trusted Application.
#
# TA_DEV_KIT_DIR is set by the Yocto recipe to the OP-TEE export directory
# installed by optee-os.
#
# To build manually (after sourcing the devkit env):
#   make TA_DEV_KIT_DIR=/path/to/optee/export-ta_arm64

TA_UUID = b1c2d3e4-f5a6-7890-abcd-ef0123456789

BINARY = $(TA_UUID)

-include $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk
