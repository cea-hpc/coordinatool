#!/bin/sh

VERSION=$(awk '/Version/ { print $2 }' coordinatool.spec)

# alternatively, meson build && ninja -C build dist can be used
git archive --prefix=coordinatool-phobos-$VERSION/ HEAD |
	xz > coordinatool-phobos-$VERSION.tar.xz

rpmbuild -ta coordinatool-phobos-$VERSION.tar.xz
