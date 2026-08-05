# SPDX-License-Identifier: BSD-2-Clause
CC      ?= gcc
CFLAGS  += -Wall -Wextra -O2
LDFLAGS += -lteec

TARGET = w77q-fs-test

$(TARGET): w77q_fs_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
