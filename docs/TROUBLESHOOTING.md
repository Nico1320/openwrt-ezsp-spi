# Troubleshooting

## Join fails: `stack status 0xAB (no beacons heard)`

The scan a moment earlier said `found it ... permit-join is open`, then the
association got nothing.

The coordinator's join window closed in between. `ezsp-spi join` runs a config
sequence after its scan — stack profile, security level, key table, endpoint
registration — and that gap is enough.

Open the window and start the join **back to back**. Do any preparation
(stopping the service, `leave`) before opening the window.

## Join fails: `pan 0xNNNN not heard on channel N`

Wrong parameters, or nothing is transmitting.

If the coordinator is behind the TCP bridge, check a client is connected. The
bridge is a stateless relay — with nothing attached the coordinator is silent
and there is no network to find. Usual cause after the client crashed or was
stopped.

To see what is actually on air:

```sh
ezsp-spi scan
```

## `gpio29 ... Resource busy`

The service is running and owns the pins:

```sh
/etc/init.d/ezsp-spi stop
```

If it persists with the service stopped, the device tree patch is not applied.
Either flash a patched image, or work around it:

```sh
uci set ezsp-spi.radio.nssel_int_mode=input
uci commit ezsp-spi && /etc/init.d/ezsp-spi restart
```

`join` and `scan` disagree on the default for this pin: `join` leaves it alone,
`scan` drives it. Pass `--nssel-int-mode input` to `scan` on an unpatched node.

## Reports show `N sent, M failed`

Failures mean the coordinator did not acknowledge. If it is behind the bridge,
the usual cause is that its client stopped running.

## Neighbours empty in LuCI

Expected in bridge mode — the client owns the network and the daemon keeps no
state to read.
