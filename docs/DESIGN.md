# ezsp-spi design notes

## nSSEL_INT

gpio29 on the V2, gpio56 on the V1. Autodetected from `/tmp/sysinfo/board_name`.

Not a second chip select — the SPI chip select is gpio45. AN711 requires
`nSSEL_INT` to be tied to nSSEL: it carries the same signal on a second,
interrupt-capable pin, because the SPI peripheral's own `nSSEL` cannot raise an
interrupt while the part is asleep. Most designs strap the two together on the
PCB; Linksys routed it to its own host GPIO, so the host drives both ends.

`nssel_int_mode=follow` drives it with nSSEL. That is the default and correct.
Leaving it undriven only works because the NCP never sleeps in this design.

On the V1 gpio29 is an eMMC line and the tool refuses to drive it.

Bootloader entry is `nWAKE` held low across reset, not this pin. The bootloader
does need `nSSEL_INT`, so NCP flashing on the V2 needs gpio29 free.

## SPI transaction framing

A transaction must end exactly on the `0xA7` terminator. Clocking past it makes
the NCP report every following transaction as aborted (`0x02`), one transaction
late.

The kernel cannot express "hold chip select across several ioctls and release on
a byte value", so the device tree drops `cs-gpios` and the host drives gpio45
itself. Where a device tree still declares `cs-gpios`, the `cs_change` path is
used instead.

## The ASH bridge

zigbee-herdsman and bellows carry the complete EZSP codec for v4–v13. Relaying
their frames untouched gets every command they implement, coordinator included,
without per-command C.

The relay never invents a frame, never rewrites a payload and never answers on
the NCP's behalf. Synthesising join announcements or canned ZDO replies is how
you get devices that appear in a coordinator and cannot be controlled.

Callback delivery is the one translation: a UART NCP pushes callbacks, an SPI
NCP must be asked. On nHOST_INT the bridge issues an EZSP `callback` (0x06) and
forwards the result as an unsolicited ASH DATA frame.

Two ordering rules:

1. Send nothing before the client's `RST` is answered with `RSTACK`, or a queued
   callback goes out as the first DATA frame.
2. Poll nothing before the client has negotiated an EZSP version. Until then the
   NCP rejects everything with `invalidCommand` (0x58) and the frame layout is
   unknown.

## EZSP configuration

Every command that touches the network goes through one shared
`ezsp_apply_config()`, so `join` and `run` cannot drift apart.

Key table size defaults to 0 on this stack and must be raised, or joining fails
with `0x94`.

## Radio power

+8 dBm is the ceiling. `setRadioPower(19)` is refused with `0x8B`.

`EZSP_CONFIG_TX_POWER_MODE` is `0x17`. `0x12` is indirect transmission timeout —
setting that to 3 ms breaks messaging to sleepy end devices.
