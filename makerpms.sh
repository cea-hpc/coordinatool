#!/bin/sh

VERSION=$(awk '/Version/ { print $2 }' coordinatool.spec)

# alternatively, meson build && ninja -C build dist can be used
git archive --prefix=coordinatool-$VERSION/ HEAD |
	xz > coordinatool-$VERSION.tar.xz

rpmbuild -ta coordinatool-$VERSION.tar.xz
