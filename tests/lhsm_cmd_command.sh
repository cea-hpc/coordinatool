#!/bin/sh

fn="$1"
fd="$2"
fid="$3"
data="$4"

ARCHIVEDIR=${ARCHIVEDIR:-/tmp/archive}
if [ ! -d "$ARCHIVEDIR" ]; then
	mkdir -p "$ARCHIVEDIR"
fi

# debug: delay archives
#while ! [[ -e /tmp/doarch ]]; do
#	sleep 1
#done

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
