#!/bin/bash

#HWCOMPOSER_SERVICES="(vendor.hwcomposer-.*|vendor.qti.hardware.display.composer)"
ANDROID_SERVICE_SINGLE="${1}"
ANDROID_SERVICE_ACTION="${2}"
ANDROID_SERVICE_STAMP_DIRECTORY="/run/android-service"
ANDROID_SERVICE_STAMP="${ANDROID_SERVICE_STAMP_DIRECTORY}/${ANDROID_SERVICE_SINGLE}-stamp"
LXC_CONTAINER_NAME="android"
DEFAULT_WAIT_TIMEOUT=60

error() {
	echo "E: ${@}" >&2
	exit 1
}

current_status() {
	getprop init.svc.${service_service}
}

start() {
	if [ "$(current_status)" == "running" ]
	then
		touch ${ANDROID_SERVICE_STAMP}
		return 0
	fi

	# Start operation is weird since it's kickstarted by Android's
	# init - thus we assume that if ${ANDROID_SERVICE_STAMP} doesn't
	# exist the startup has already been triggered.
	#
	# If it does exist, instead, we should indeed start the service by
	# ourselves.
	if [ -e "${ANDROID_SERVICE_STAMP}" ]; then
		android_start ${service_service}
	fi

	if [ -z $WAIT_TIMEOUT ] ; then
		WAIT_TIMEOUT=$DEFAULT_WAIT_TIMEOUT
	fi

	# Now, wait
	echo "Waiting ${service_service} for $WAIT_TIMEOUT seconds"
	timeout $WAIT_TIMEOUT waitforservice init.svc.${service_service}
	(( $? != 0 )) && echo "Failed to start ${service_service}"

	# Once we return, create the stamp file
	touch ${ANDROID_SERVICE_STAMP}
}

stop() {
	[ "$(current_status)" == "stopped" ] && return 0

	# Try to gracefully stop via the Android-provided facilities
	android_stop ${service_service}

	if [ "${ANDROID_SERVICE_FORCE_KILL}" != "yes" ]; then
		WAITFORSERVICE_VALUE="stopped" timeout 5 waitforservice init.svc.${service_service}
	fi

	if [ "${ANDROID_SERVICE_FORCE_KILL}" == "yes" ] ; then
		pid=$(lxc-attach -n ${LXC_CONTAINER_NAME} -- /bin/pidof ${service_process} \;)
		[ "${pid}" != "" ] && android_kill -9 ${pid}
		setprop init.svc.${service_service} stopped
	fi
}

ANDROID_SERVICE=${ANDROID_SERVICE:-${ANDROID_SERVICE_SINGLE}}

service=$(grep -roP "service ${ANDROID_SERVICE} /.*" /system/etc/init /vendor/etc/init | head -n 1)
if [ -z "${service}" ]; then
	error "Unable to detect service"
fi

service_service=$(echo ${service} | awk '{ print $2 }')
service_path=$(echo ${service} | awk '{ print $3 }')
service_process=$(echo ${service_path} | awk -F "/" '{print $NF}')

mkdir -p "${ANDROID_SERVICE_STAMP_DIRECTORY}"

case "${ANDROID_SERVICE_ACTION}" in
	"start")
		start
		;;
	"stop")
		stop
		;;
	"restart")
		stop
		start
		;;
	*)
		error "USAGE: ${0} <service> start|stop|restart"
		;;
esac
