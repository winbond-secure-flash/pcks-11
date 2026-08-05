# SPDX-License-Identifier: BSD-2-Clause
#
# core/drivers/spi/qlib/sub.mk
#
# QLIB stack linked into OP-TEE Secure World core (CFG_W77Q_QLIB=y).
#
# QLIB is delivered as a prebuilt aarch64 static library (libqlib.a) plus
# public API headers.  Only the integration glue is compiled from source;
# the QLIB core/utils object code is pulled from the archive at link time.
#
# Layout (populated by the Yocto optee-os recipe do_configure:prepend):
#   inc/    — Public API:   qlib.h, qlib_platform.h, qlib_types.h, ...
#   defs/   — Portability:  defs.h, defs_types.h, defs_os_general.h, ...
#   lib/    — Prebuilt lib:  libqlib.a (aarch64 static library)
#
# Integration files in this directory (installed by recipe):
#   w77q_qlib.c             — w77q_read/write/erase_sector → QLIB_*_LA
#   qlib_platform.c         — PLAT_SPI/HASH/NONCE callbacks
#   w77q_qlib_provision.c   — provisioning helper (CFG_W77Q_QLIB_PROVISION=y)

# ---- Include paths -------------------------------------------------------
# inc/   — QLIB public API headers (qlib.h, qlib_platform.h, …)
# defs/  — Winbond portability layer (defs.h, defs_types.h, …)

incdirs-y += inc
incdirs-y += defs
incdirs-y += .

# W77Q51NW (512Mbit) has flashSize=0x1A vs dieSize=0x18 → QLIB computes
# MaxDieId = (1<<(0x1A-0x18))-1 = 3 (four logical dies of 128Mbit each).
# QLIB_MAX_DIES_SUPPORTED defaults to 1; valid values are 1/2/4/8.
cflags-y += -DQLIB_MAX_DIES_SUPPORTED=4

# ---- Integration files ---------------------------------------------------
srcs-y += w77q_qlib.c
srcs-y += qlib_platform.c
# Provisioning helper — compiled only when CFG_W77Q_QLIB_PROVISION=y
srcs-$(CFG_W77Q_QLIB_PROVISION) += w77q_qlib_provision.c

# ---- Prebuilt QLIB static library ----------------------------------------
# libqlib.a is added to the core link (link.mk: link-ldadd += $(libdeps)).
#
# Ordering on the link line:
#   <core objs ...> <archives ...>
# The integration objects (w77q_qlib.c.o, qlib_platform.c.o) are always
# linked core objects placed before every archive.  w77q_qlib.c.o pulls the
# QLIB_* members out of libqlib.a, and qlib_platform.c.o already satisfies
# the archive's PLAT_* back-references.
#
# libqlib.a itself references OP-TEE core helpers (memcpy/memset/... from
# libutils.a), so it must be processed *before* the other archives.
# mk/lib.mk builds libdeps as a simply-expanded variable and prepends each
# library, so we prepend libqlib.a here to place it first among the archives.
# $(sub-dir) expands to this directory relative to the OP-TEE source root
# (core/drivers/spi/qlib) and is captured immediately by the := assignment.
libdeps := $(sub-dir)/lib/libqlib.a $(libdeps)
