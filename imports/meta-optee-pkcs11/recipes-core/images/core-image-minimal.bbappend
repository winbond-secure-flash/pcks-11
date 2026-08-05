# Upstream OP-TEE + PKCS#11 packages
IMAGE_INSTALL:append = " \
    optee-examples \
    optee-os-pkcs11 \
    libckteec \
    opensc \
"
