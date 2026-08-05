# OP-TEE machine-specific settings for sparrow-hawk (R-Car V4H / r8a779g3)
#
# PLATFORM=rcar is the upstream OP-TEE platform for all Renesas R-Car SoCs.
# PLATFORM_FLAVOR=spider_s4 is the only Gen4 flavor in upstream OP-TEE; it
# forces CFG_RCAR_GEN4=y (correct GIC base, HSCIF0 UART, GICv3, cluster
# config) which matches V4H hardware.  A dedicated v4h flavor can be added
# upstream later if needed.
PLATFORM        = "rcar"
PLATFORM_FLAVOR = "spider_s4"
