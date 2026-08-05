# OP-TEE OS – upstream project, tag 4.5.0 (supports PLATFORM=rcar, Gen3 & Gen4)
PV:pn-optee-os              = "4.5.0+git${SRCPV}"
BRANCH:pn-optee-os          = "master"
SRCREV:pn-optee-os          = "0919de0f7c79ad35ad3c8ace5f823ad1344b4716"
SRC_URI:pn-optee-os         = "git://github.com/OP-TEE/optee_os.git;branch=${BRANCH};protocol=https"

# OP-TEE Client – upstream project
PV:pn-optee-client          = "4.5.0+git${SRCPV}"
BRANCH:pn-optee-client      = "master"
SRCREV:pn-optee-client      = "9d6f69844ff60ec0966cf3659abcc38eda8b31ea"
SRC_URI:pn-optee-client     = "git://github.com/OP-TEE/optee_client.git;branch=${BRANCH};protocol=https"
