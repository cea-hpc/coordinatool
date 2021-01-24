#!/bin/sh

fn="$1"
fid="$2"
fd="$3"

while ! [[ -e /tmp/doarch ]]; do
	sleep 1
done

case "$fn" in
	archive)
		dd if="/proc/self/fd/$fd" of="/tmp/arch/$fid" bs=1M
		;;
	restore)
		dd if="/tmp/arch/$fid" of="/proc/self/fd/$fd" bs=1M
		;;
	remove)
		rm -f "/tmp/arch/$fid"
		;;
	*)
		echo "Wrong arg!"
		exit 1
		;;
esac
