#!/bin/sh
# Build the vendor blob tarball from a WHW03 firmware image and install it on a
# router, so that flashing an NCP image there is a single command.
#
#   ./deploy-vendor.sh <rootfs-dir> <router>
#   ./deploy-vendor.sh /path/to/extracted/rootfs root@192.0.2.1
#
# The blob is Linksys' TestNCP_SPI, which wraps Silicon Labs' EBL transfer
# code. It is not redistributable, so it never enters this repo or a package --
# extract it from firmware for a device you own and this pushes it straight to
# the router.
#
# Afterwards, on the router:
#
#   ezsp-spi-flash /root/ncp-fw/<image>.ebl

set -e

ROOTFS="$1"
TARGET="$2"
HERE="$(dirname "$0")"
TARBALL="${TMPDIR:-/tmp}/zbroot.$$.tar.gz"

usage() {
	cat <<USAGE
usage: $0 <rootfs-dir> <router>

  rootfs-dir  an extracted WHW03 firmware root (binwalk, plus ubi_reader for
              the V2). Must contain usr/sbin/TestNCP_SPI.
  router      ssh destination, e.g. root@192.0.2.1

Then on the router, with an .ebl in /root/ncp-fw:
  ezsp-spi-flash /root/ncp-fw/<image>.ebl
USAGE
	exit 1
}

[ -n "$ROOTFS" ] && [ -n "$TARGET" ] || usage
[ -d "$ROOTFS" ] || { echo "no such directory: $ROOTFS" >&2; exit 1; }

trap 'rm -f "$TARBALL"' EXIT INT TERM

echo "==> building the vendor tarball"
"$HERE/mkvendor.sh" "$ROOTFS" "$TARBALL"

echo "==> checking the router"
ssh "$TARGET" 'command -v ezsp-spi-flash >/dev/null' || {
	echo "ezsp-spi-flash not found on $TARGET -- install the ezsp-spi package first" >&2
	exit 1
}

# scp is absent on stock OpenWrt (dropbear ships no scp binary).
echo "==> copying $(du -h "$TARBALL" | cut -f1) to $TARGET:/root/zbroot.tar.gz"
ssh "$TARGET" 'cat > /root/zbroot.tar.gz' < "$TARBALL"

echo "==> verifying"
ssh "$TARGET" '
	set -e
	n=$(tar tzf /root/zbroot.tar.gz | grep -cE "^bin/busybox$|TestNCP_SPI|zbbootloader")
	[ "$n" -ge 3 ] || { echo "tarball incomplete on the router" >&2; exit 1; }
	mkdir -p /root/ncp-fw
	echo "  ok: $(ls -lh /root/zbroot.tar.gz | awk "{print \$5}")"
	echo "  images present:"
	ls -1 /root/ncp-fw/*.ebl 2>/dev/null | sed "s|^|    |" || echo "    (none yet -- copy an .ebl into /root/ncp-fw)"
'

cat <<DONE

==> done. On the router:

      ezsp-spi-flash /root/ncp-fw/<image>.ebl

    It verifies the image matches the chip before writing, stops and restarts
    the router service itself, and reports the radio state afterwards.
DONE
