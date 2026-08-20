# Configuration

All settings live in `/etc/config/ezsp-spi`. The package ships a fully
commented default; after an upgrade the new one is written to
`/etc/config/ezsp-spi.apk-new` and your existing file is left alone.

## Modes

`radio.mode` selects what the node does with the radio. The two are mutually
exclusive — one process owns the hardware.

| mode | what it does |
|---|---|
| `router` (default) | joins an existing Zigbee network and repeats for it |
| `bridge` | serves the radio over TCP as an ASH endpoint |

## Router mode

```sh
uci set ezsp-spi.radio.enabled=1
uci set ezsp-spi.radio.mode=router
uci set ezsp-spi.network.channel=25
uci set ezsp-spi.network.pan_id=4920        # decimal, 0x1338
uci commit ezsp-spi
/etc/init.d/ezsp-spi restart
```

`pan_id` is decimal in UCI and hex on the command line. Take both values from
your coordinator.

Joining needs permit-join open on the coordinator:

```sh
/etc/init.d/ezsp-spi stop
ezsp-spi join --channel 25 --pan-id 0x1338
/etc/init.d/ezsp-spi start
```

Open the coordinator's join window **immediately before** running `join`, not
minutes earlier. See TROUBLESHOOTING.md.

## Bridge mode

```sh
uci set ezsp-spi.radio.mode=bridge
uci set ezsp-spi.bridge.port=8888
uci set ezsp-spi.bridge.bind=0.0.0.0
uci commit ezsp-spi
/etc/init.d/ezsp-spi restart
```

The bridge is a stateless relay: it forwards EZSP frames and owns no network
state. **The coordinator is only on the air while a client is connected** and
has run `networkInit`. With nothing attached the radio is silent — devices
scanning will not find the network. This is by design, and is why the client
(Home Assistant) must run continuously.

There is **no authentication on this port**. Whoever connects owns your Zigbee
network. Keep it off untrusted networks, or restrict it:

```sh
uci add firewall rule
uci set firewall.@rule[-1].name=zigbee-bridge
uci set firewall.@rule[-1].src=lan
uci set firewall.@rule[-1].proto=tcp
uci set firewall.@rule[-1].dest_port=8888
uci set firewall.@rule[-1].src_ip=192.0.2.10     # the client's address
uci set firewall.@rule[-1].target=ACCEPT
uci commit firewall && /etc/init.d/firewall restart
```

## Radio pins

Board specific, autodetected from `/tmp/sysinfo/board_name`. Leave unset unless
you know otherwise.

| option | V2 | V1 |
|---|---|---|
| `reset` | 49 | 49 |
| `wake` | 31 | 55 |
| `hostint` | 50 | 50 |
| `nssel_int` | 29 | 56 |

## Other options

```
option speed '1000000'          # SPI clock; vendor firmware uses 12000000
option report_secs '300'        # Basic-cluster attribute report interval, 0 = off
option ezsp_version '4'         # version to request first; 8 for EmberZNet 6.7.8
option socket '/var/run/ezsp-spi.sock'
```

`report_secs` is what gives the coordinator frames to take a link quality
reading from. Reports are jittered so nodes restored together do not settle
into lockstep. Drop it to 60 while measuring, then put it back.

## Status

```sh
ezsp-spi status        # queries the running daemon, does not reset the NCP
ezsp-spi info          # resets the NCP; stop the service first
ezsp-spi neighbors     # neighbour table, children, routes
ezsp-spi scan          # active scan for networks on air
```

`status` goes through the control socket and is safe while the service is up.
Everything else needs the GPIOs, so stop the service first — the gpio chardev
is exclusive.
