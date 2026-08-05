# W77Q-specific packages (QLIB, provisioning, tests)
IMAGE_INSTALL:append:sparrow-hawk = " \
    secure-counter \
    pkcs11-app \
    pkcs11-tests \
    tee-storage \
    tee-demo \
    w77q-fs-test \
    vim-xxd \
"
