# openwrt-ezsp-spi

Use the Silicon Labs EM3581 Zigbee NCP in a Linksys Velop WHW03 under OpenWrt.

Two modes:

* **router** — join an existing Zigbee network and repeat for it
* **bridge** — serve the radio over TCP as ASH, so Home Assistant (ZHA) can
  drive it as a **coordinator**

Prior to this the radio had never been used outside Linksys' own firmware.

## Status

| | |
|---|---|
| Router mode | working, in production |
| Coordinator via ZHA | verified on hardware |
| WHW03 V2 | tested |
| WHW03 V1 | untested — pin map is autodetected but no unit has run it |

Built against OpenWrt 25.12, target `ipq40xx/generic`, kernel 6.12,
EmberZNet 6.7.8 / EZSP v8.

## Images

Prebuilt in [`images/`](images/).

| file | device |
|---|---|
| `…linksys_whw03-squashfs-sysupgrade.bin` | V1, upgrade from OpenWrt |
| `…linksys_whw03-squashfs-factory.bin` | V1, first install from stock |
| `…linksys_whw03v2-squashfs-sysupgrade.bin` | V2, upgrade from OpenWrt |
| `…linksys_whw03v2-squashfs-factory.bin` | V2, first install from stock |

`.manifest` files list the exact package versions in each image.

The V1 images are **untested** — built from the same tree, never run on
hardware.

## Contents

```
package/ezsp-spi/            daemon and CLI
package/luci-app-ezsp-spi/   LuCI pages under Services -> Zigbee
patches/                     device tree patch (required)
images/                      prebuilt firmware
tools/                       ASH smoke test, vendor blob helpers
docs/                        build, NCP firmware, configure, Home Assistant
```

## Building it yourself

```sh
git clone https://github.com/Nico1320/openwrt-ezsp-spi.git
git clone https://git.openwrt.org/openwrt/openwrt.git -b openwrt-25.12
cd openwrt
```

### Applying the patch

Required — the stock device tree hogs the GPIOs the NCP needs. The two
revisions differ:

| revision | gpio29 | |
|---|---|---|
| V1 | stays hogged | it is an eMMC line there |
| V2 | freed | it is `nSSEL_INT`, and NCP flashing needs it |

From the **root of the OpenWrt tree**, not from `target/`. Check before
applying:

```sh
git apply --check ../openwrt-ezsp-spi/patches/0001-*.patch
git apply         ../openwrt-ezsp-spi/patches/0001-*.patch
```

Without git, or on a tarball:

```sh
patch -p1 < ../openwrt-ezsp-spi/patches/0001-*.patch
```

The patch is against `openwrt-25.12`. On a different branch the DTS paths may
have moved — on `main` they are in `target/linux/ipq40xx/dts/` — and it will
need rebasing.

### Feeds

```sh
echo "src-link ezsp $(pwd)/../openwrt-ezsp-spi/package" >> feeds.conf.default
./scripts/feeds update -a && ./scripts/feeds install -a
```

`src-link` needs an **absolute** path.

See [docs/BUILD.md](docs/BUILD.md) for prerequisites and known build issues.

## Home Assistant, not zigbee2mqtt

zigbee2mqtt's `ember` driver needs EmberZNet 7.4+, which only runs on EFR32
silicon. The EM3581 is an EM35x part and cannot run that firmware at any
version. zigbee2mqtt 1.42.0 was the last release with the legacy `ezsp` driver.

ZHA has no such limit — zigpy and bellows maintain EZSP v4–v8 for exactly this
hardware. See [docs/HOMEASSISTANT.md](docs/HOMEASSISTANT.md).

## Vendor blob

`ezsp-spi-flash` wraps Silicon Labs' own EBL transfer code, which ships inside
the Linksys firmware as `TestNCP_SPI`. That binary is proprietary and is **not**
distributed here. It is only needed to change the NCP firmware, not to run the
radio. [docs/FLASH.md](docs/FLASH.md) covers extracting it from a stock image.

## Licence

GPL-2.0. The vendor blob is not covered and is not included.
