#!/bin/bash

error() {
	echo "E: $@" >&2
	exit 1
}

TARGET_BINARY="${0/android_/}"
LXC_CONTAINER_NAME="android"
LXC_CONTAINER_PATH="/var/lib/lxc/${LXC_CONTAINER_NAME}/rootfs"
ANDROID_SEARCH_PATH="${LXC_CONTAINER_PATH}/system/bin ${LXC_CONTAINER_PATH}/system/xbin ${LXC_CONTAINER_PATH}/rootfs/vendor/bin/"

########################################################################

[ "${UID}" == 0 ] || error "This wrapper must be run from root"
[ -e "${LXC_CONTAINER_PATH}" ] || error "Unable to find LXC container"

found_path=$(whereis -b -B ${ANDROID_SEARCH_PATH} -f ${TARGET_BINARY} | head -n 1 | awk '{ print $2 }')

[ -n "${found_path}" ] || error "Unable to find ${TARGET_BINARY}"

# Unset eventual LD_PRELOAD
unset LD_PRELOAD

# Finally execute
exec /usr/bin/lxc-attach -n ${LXC_CONTAINER_NAME} -- ${found_path/${LXC_CONTAINER_PATH}/} ${@}
