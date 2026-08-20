# Flashing NCP firmware

Changes the EmberZNet firmware on the radio itself. Not needed to run the
radio, only to change its version.

Requires the proprietary vendor blob, which is not distributed here.

## What goes where

| path | what |
|---|---|
| `/root/zbroot.tar.gz` | the vendor blob |
| `/root/ncp-fw/*.ebl` | firmware images |

`sysupgrade` wipes `/root`. Add both to `/etc/sysupgrade.conf` to keep them.

## Getting the blob

Extract a stock WHW03 firmware image first — `binwalk`, plus `ubi_reader` for
the V2. The result must contain `usr/sbin/TestNCP_SPI`.

Push it straight to a running node:

```sh
./tools/deploy-vendor.sh /path/to/extracted/rootfs root@NODE
```

Or bake it into an image you build:

```sh
./tools/mkvendor.sh /path/to/extracted/rootfs
```

That writes `package/ezsp-spi/files/zbroot.tar.gz`, which the package Makefile
picks up if present. It is gitignored and never leaves your machine.

## Using it

With no arguments it lists the images it can find and where the blob belongs:

```sh
ezsp-spi-flash
```

The daemon owns the radio, so stop it first:

```sh
/etc/init.d/ezsp-spi stop
ezsp-spi-flash /root/ncp-fw/NCP_SPI_EM3581_678.ebl
/etc/init.d/ezsp-spi start
```

The argument is a path, not a bare filename.

It checks the image is built for this chip before transferring, so a wrong one
is refused rather than half-written. Once it reports a size, do not interrupt
it.

To check an image without flashing:

```sh
ezsp-spi verify /root/ncp-fw/NCP_SPI_EM3581_678.ebl
```

## Requirements

Needs the device tree patch applied. Without it the transfer hangs at
`Starting bootloader communications`, or a GPIO export returns `EBUSY`.

An interrupted transfer can be retried. The bootloader is in its own flash
region and runs before the app on every reset, so keep a known-good `.ebl` in
`/root/ncp-fw`.
