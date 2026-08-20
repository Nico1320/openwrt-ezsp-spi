#!/bin/sh
# Build the vendor support tarball used by ezsp-spi-flash, from a WHW03 rootfs
# you extracted yourself.
#
#   ./mkvendor.sh /path/to/extracted/rootfs [output.tar.gz]
#
# Without an output path the result lands in
# package/ezsp-spi/files/zbroot.tar.gz, which the package Makefile picks up
# automatically if present. To push it straight to a router instead, use
# deploy-vendor.sh.
#
# It is deliberately NOT committed or distributed. TestNCP_SPI is a Linksys
# application built on Silicon Labs' EmberZNet stack: proprietary, and not
# covered by the GPL obligations that apply to the kernel and busybox in the
# same firmware. Extracting it from firmware for a device you own is fine;
# redistributing it is not ours to do.
set -e

R="$1"
OUT="${2:-$(dirname "$0")/../package/ezsp-spi/files/zbroot.tar.gz}"

[ -n "$R" ] || { echo "usage: $0 <extracted-whw03-rootfs> [output.tar.gz]" >&2; exit 1; }
[ -d "$R" ] || { echo "not a directory: $R" >&2; exit 1; }

for f in usr/sbin/TestNCP_SPI usr/sbin/zbbootloader.sh bin/busybox; do
	[ -f "$R/$f" ] || { echo "missing $f under $R -- is that a WHW03 rootfs?" >&2; exit 1; }
done

mkdir -p "$(dirname "$OUT")"

# bin/busybox and bin/sh matter: the vendor binary shells out to syscfg, and
# only a uClibc-linked shell can run inside the chroot. With the host's musl
# busybox the call fails silently and the binary falls back to the V1 pin map.
( cd "$R" && tar czf - \
	lib usr/lib \
	bin/busybox bin/sh \
	usr/sbin/TestNCP_SPI \
	usr/sbin/zbbootloader.sh \
	usr/sbin/devmem2 \
	2>/dev/null ) > "$OUT"

echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
[ -n "$2" ] || echo "rebuild the package and it will be installed to /root/zbroot.tar.gz"
