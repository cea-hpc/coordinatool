#!/bin/sh

fn="$1"
fd="$2"
fid="$3"
data="$4"

ARCHIVEDIR=${ARCHIVEDIR:-/tmp/archive}
if [ ! -d "$ARCHIVEDIR" ]; then
	mkdir -p "$ARCHIVEDIR"
fi

if [ -n "$WAIT_FILE" ]; then
	while ! [ -e "$WAIT_FILE" ]; do
		sleep 1
	done
fi

# only use $data in path if requested
[ -n "$CTDATA_PATH" ] || data=""

case "$fn" in
	archive)
		dd if="/proc/self/fd/$fd" of="$ARCHIVEDIR/$data$fid" bs=1M
		;;
	restore)
		dd if="$ARCHIVEDIR/$data$fid" of="/proc/self/fd/$fd" bs=1M
		;;
	remove)
		rm -f "$ARCHIVEDIR/$data$fid"
		;;
	*)
		echo "Wrong arg!"
		exit 1
		;;
esac
