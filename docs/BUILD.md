# Building

OpenWrt 25.12 (`openwrt-25.12` branch), target `ipq40xx/generic`, profile
`DEVICE_linksys_whw03v2`.

## Build host prerequisites

Debian / Ubuntu:

```sh
sudo apt update
sudo apt install -y build-essential clang flex bison g++ gawk \
  gcc-multilib g++-multilib gettext git libncurses-dev libssl-dev libelf-dev \
  python3-setuptools python3-dev python3-pyelftools rsync swig unzip \
  zlib1g-dev file wget ccache qemu-utils subversion time xsltproc zstd \
  device-tree-compiler python3-distutils-extra
```

Not `python3-distutils` — removed in Python 3.12, no install candidate on a
current distro. `python3-setuptools` covers it.

Build on a case-sensitive filesystem. The kernel tree has headers differing
only by case.

## Feeds

```sh
cd openwrt
./scripts/feeds update -a
./scripts/feeds install -a
```

`feeds.conf.default` carries two `src-link` lines pointing at the local
`feed-zigbee` and `feed-bluetooth` directories. They use absolute paths — if
you move the tree, rewrite them, then regenerate:

```sh
rm -rf feeds package/feeds tmp
./scripts/feeds update -a && ./scripts/feeds install -a
```

## Configure and build

`DEVICE_linksys_whw03` for a V1, `DEVICE_linksys_whw03v2` for a V2:

```
CONFIG_TARGET_ipq40xx=y
CONFIG_TARGET_ipq40xx_generic=y
CONFIG_TARGET_ipq40xx_generic_DEVICE_linksys_whw03v2=y

CONFIG_PACKAGE_kmod-ath10k=y
# CONFIG_PACKAGE_kmod-ath10k-ct is not set
CONFIG_PACKAGE_ath10k-firmware-qca4019=y
# CONFIG_PACKAGE_ath10k-firmware-qca4019-ct is not set
CONFIG_PACKAGE_ath10k-firmware-qca9888=y
# CONFIG_PACKAGE_ath10k-firmware-qca9888-ct is not set

CONFIG_PACKAGE_wpad-openssl=y
# CONFIG_PACKAGE_wpad-basic-mbedtls is not set

CONFIG_PACKAGE_ezsp-spi=y
CONFIG_PACKAGE_luci-app-ezsp-spi=y
CONFIG_PACKAGE_kmod-spi-dev=y
CONFIG_PACKAGE_luci=y
```


```sh
make defconfig
grep -q '^CONFIG_PACKAGE_ezsp-spi=y' .config || {
    printf 'CONFIG_PACKAGE_ezsp-spi=y\nCONFIG_PACKAGE_luci-app-ezsp-spi=y\n' >> .config
    make defconfig
}
grep -E '^CONFIG_PACKAGE_(ezsp-spi|luci-app-ezsp-spi)=' .config   # must print both

make -j8 download
nice make -j8
```

Images land in `bin/targets/ipq40xx/generic/`.

## Packages only

To rebuild just the Zigbee packages against an existing tree:

```sh
make package/ezsp-spi/{clean,compile} package/luci-app-ezsp-spi/{clean,compile} -j8
ls bin/packages/*/zigbee/*.apk
```

If a parallel build fails without printing a compiler diagnostic, rerun it
single-threaded before assuming the code is at fault:

```sh
make package/ezsp-spi/compile -j1 V=s
```

## Packages the image needs

```
ezsp-spi            the daemon and CLI
kmod-spi-dev        dependency of ezsp-spi
luci-app-ezsp-spi   the web UI
luci                needed for luci-app-ezsp-spi to have anywhere to render
```
