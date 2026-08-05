# SPDX-License-Identifier: BSD-2-Clause
#
# Makefile for the W77Q TEE demo host application.
#
# TEEC_EXPORT is set by the Yocto recipe to the optee-client sysroot.
# To build manually:
#   make TEEC_EXPORT=/path/to/optee-client-export TA_INCLUDE=../ta/include

TEEC_EXPORT ?= /usr
TA_INCLUDE  ?= ../ta/include

CC      ?= $(CROSS_COMPILE)gcc
CFLAGS  += -Wall -Wextra -O2
CFLAGS  += -I$(TEEC_EXPORT)/include
CFLAGS  += -I$(TA_INCLUDE)
LDFLAGS += -L$(TEEC_EXPORT)/lib -lteec

BINARY = tee-demo

$(BINARY): tee-demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BINARY)

.PHONY: clean
