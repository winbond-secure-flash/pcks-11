# Fix bsp-info build failure when layers are not git repositories
# (e.g. when building from a release tarball rather than git clones)

do_install() {
    install -d ${D}${sysconfdir}/

    # Write BSP version
    printf "BSP version: ${BSP_VERSION}\n\n" >> ${D}${sysconfdir}/bspinfo

    # Yocto version and codename
    printf "${DISTRO_NAME} " >> ${D}${sysconfdir}/bspinfo

    distro_version_nodate=${@'${DISTRO_VERSION}'.replace('snapshot-${DATE}','snapshot').replace('${DATE}','')}
    printf "%s " $distro_version_nodate >> ${D}${sysconfdir}/bspinfo

    printf "(${DISTRO_CODENAME})" >> ${D}${sysconfdir}/bspinfo
    echo >> ${D}${sysconfdir}/bspinfo

    # Linux kernel
    printf "Linux kernel: ${KERNEL_VERSION} " >> ${D}${sysconfdir}/bspinfo
    echo >> ${D}${sysconfdir}/bspinfo

    # Adding Yocto layer information
    echo "" >> ${D}${sysconfdir}/bspinfo

    # Other layers
    for layer in `ls -d ${TOPDIR}/../poky ${TOPDIR}/../meta-*`; do
        LAYER_NAME=$(basename $layer)
        BRANCH=$(git -C ${layer} branch 2>/dev/null | grep \* | awk '{print $2}' || true)
        COMMIT=$(git -C ${layer} rev-parse HEAD 2>/dev/null || true)
        BRANCH=${BRANCH:-"release"}
        COMMIT=${COMMIT:-"unknown"}

        # Output results if layer is used
        if grep -q ${LAYER_NAME} ${TOPDIR}/conf/bblayers.conf; then
            echo "${LAYER_NAME}=\"${BRANCH}:${COMMIT}\"" >> ${D}${sysconfdir}/bspinfo
        fi
    done
}
