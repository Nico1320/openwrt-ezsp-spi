# Home Assistant (ZHA)

ZHA drives the radio as a **coordinator** over the TCP bridge.

## Use ZHA, not zigbee2mqtt

zigbee2mqtt is a dead end for this hardware.

Its modern `ember` driver requires EmberZNet 7.4 or newer, which only runs on
EFR32 silicon. The EM3581 is a Cortex-M3 EM35x part — it cannot run that
firmware at any version. Attempting it gives:

```
Adapter EZSP protocol version (8) is not supported by Host [13-14]
```

The legacy `ezsp` driver works, but 1.42.0 is the last zigbee2mqtt release that
carries it.

ZHA has no such limit: zigpy and bellows maintain EZSP v4–v8 specifically for
EM35x hardware.

## Setup

Put the node in bridge mode first (see CONFIGURE.md), then:

```
Settings → Devices & Services → Add Integration → Zigbee Home Automation
Radio type:  EZSP
Path:        socket://NODE:8888
```

Choose the manual path entry if offered a serial-port list — the radio is over
TCP, not a local device.

ZHA will form a new network on first run. It picks its own PAN ID and channel;
check what it chose before joining anything to it:

```
Settings → Devices & Services → ZHA → Configure → Network settings
```

If it lands on the same channel as an existing Zigbee network of yours, move it.
Two networks sharing a channel contend for airtime.

## Joining a router node

ZHA's "Add device" screen opens the join window. Run the join on the node while
that window is open:

```sh
ssh root@NODE '/etc/init.d/ezsp-spi stop'
ssh root@NODE 'ezsp-spi join --channel CH --pan-id 0xPAN'
ssh root@NODE 'uci set ezsp-spi.network.channel=CH; uci set ezsp-spi.network.pan_id=DEC; uci commit ezsp-spi'
ssh root@NODE '/etc/init.d/ezsp-spi start'
```

To find the network without guessing, scan from the joining node:

```sh
ezsp-spi scan --nssel-int-mode input | grep -A4 'allowing join   YES'
```

## Keep it running

ZHA holds the bridge connection open permanently, which keeps the coordinator on
the air. If Home Assistant stops, the radio goes silent and joined devices
report failures until it returns. Run it under systemd, not in a terminal.

## Verifying it actually works

A device appearing in the UI is not proof. Devices can be listed from
announcements without anything ever having talked to them. Look for an addressed
unicast with a delivery confirmation in the Home Assistant log:

```
sendUnicast:        status SUCCESS
indexOrDestination: <the node's short address in decimal>
apsFrame:           profileId=260, clusterId=0
messageSentHandler: status EmberStatus.SUCCESS
```

From the node's side, `ezsp-spi status` should show `state: joined` and a
`reports: N sent, 0 failed` counter that increases.
