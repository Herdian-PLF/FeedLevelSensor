# Silometer LoRa point-to-point protocol, v0.1

**Status:** draft, implemented in `embedded/silometer_endpoint/` and `embedded/gateway_arduinounoq`
**Date:** 2026-09-22

A star network: many endpoints, one gateway with internet access. Point-to-point,
not LoRaWAN — the E32-900T20D is a UART-fronted SX1276 and exposes no LoRaWAN
stack, and no bare RF module is available. The application framing below is kept
independent of the radio's own addressing so a later move to LoRaWAN changes the
transport and not the payload.

This document is the reference for both halves.

## Radio layer

Both ends are E32-900T20D modules configured identically apart from their address.
Register semantics are from `E32-900T20D_UserManual_EN_v1.3.pdf` section 7.5 not in
this repo.

| Register | Value | Meaning |
|---|---|---|
| `ADDH`/`ADDL` | endpoint: its own ID; gateway: `0x0001` | module address |
| `SPED` | `0x1A` | 8N1, UART 9600, air data rate 2.4 kbps |
| `CHAN` | `0x35` | carrier = 862 MHz + CHAN = 915 MHz |
| `OPTION` | `0xC4` | fixed transmission, push-pull IO, FEC on, 20 dBm |

**Air rate.** `SPED[2:0]` comes from `cfg::kAirRate`. Every air-time-bound timer on
both ends derives from it (see [Timers and retry policy](#timers-and-retry-policy)),
so a range sweep changes only that constant. 2.4 kbps is the slowest rate these
modules accept: the bench units (C3 model `0x32`, version `0x82`) reject 0.3k and
1.2k although the manual lists them. The `C0` echo returns the old `SPED`, and the
readback after a `C4` reset confirms it. 4.8k is accepted, so the module rejects
only the rates below 2.4k; the driver and the write path are not at fault.

Parameters are written with `C0` (saved across power-down) in Mode 3, which always
runs at 9600 8N1 regardless of the baud selected in `SPED`. Commands and mode
switches both wait on AUX: the module holds it low while busy and drops anything
sent in that window, and a mode switch only takes effect once AUX has been high
for 2 ms (manual sections 5.6.4 and 6.1).

**Addressing is done by the radio.** With `OPTION` bit 7 set, the first three bytes
written to the module are the target `ADDH`, `ADDL`, `CHAN`; the module consumes
them as routing, transmits with that target, and reverts to its own settings
afterwards. A downlink addressed to one endpoint therefore never reaches another
endpoint's UART. The application header repeats the source address anyway, so the
guarantee does not depend on a single vendor behaviour.

`0x0000` and `0xFFFF` are reserved: the manual (sections 5.3 and 5.4) makes a
module holding either address receive every frame on its channel.

**Channel constraint.** ANATEL grants 902–907.5 and 915–928 MHz, so `CHAN` must be
`0x28`–`0x2D` or `0x35`–`0x42`. This is enforced at compile time and again on any
channel the gateway pushes.

**Packet size.** The manual gives 58 bytes as the maximum single air package but does
not say whether the three routing bytes count against it. Measured on the bench with
`embedded/silometer_endpoint/src/probe/`: the routing bytes **do** count against the 58,
leaving 55 bytes of usable payload,and the receiver strips them before the data reaches
its UART. Writes past that are **truncated, not sub-packed**, which is a different behavior
then described in the component manual. `cfg::kMaxAirPayload = 58` with `cfg::kRoutingBytes = 3`
is correct as implemented — raising it would put every frame over 55 bytes on the wire
incomplete, to fail CRC at the far end.

## Application frame

Every frame, in both directions, has this shape. Multi-byte fields are little-endian.

```
offset  size  field
  0      1    MAGIC0   0xA5
  1      1    MAGIC1   0x5A
  2      1    VER (high nibble, 0x1) | TYPE (low nibble)
  3      2    SRC      sender's 16-bit address
  5      1    SEQ      cycle sequence number, wraps at 256
  6      1    FRAG     index (high nibble) | count (low nibble); 0x00 when not DATA
  7      1    LEN      payload length in bytes
  8    LEN    payload
8+LEN    2    CRC16    over bytes 0 .. 7+LEN
```

10 bytes of overhead. CRC-16/CCITT-FALSE: polynomial `0x1021`, initial value
`0xFFFF`, no reflection, no final XOR.

A receiver discards a frame that fails magic, version, length or CRC, and — for the
endpoint — one whose `SRC` is not the gateway or whose `SEQ` is not the current
cycle's. **Discarding never closes the receive window**: a corrupted or
mis-addressed frame must not consume a transmission attempt.

### Fragment geometry

Derived at compile time from the air-packet budget, so there is one number to change:

```
frame budget      58 − 3 routing          = 55 bytes
payload budget    55 − 10 header and CRC  = 45 bytes
zone size         uint16 distance + uint8 SNR = 3 bytes
zones per frame   45 / 3                  = 15 max
fragments         ceil(64 / 15)           = 5
zones per frag    ceil(64 / 5)            = 13   (the last carries 12)
frame on the wire 13 × 3 + 10             = 49 bytes
```

Fragment *i* carries zones `13i` through `13i + 12`, row-major over the 8×8 grid.

## Message types

### `0x1 HELLO` — endpoint to gateway

12-byte payload. Telemetry rides here rather than in the data burst so it still
reaches the gateway when the burst fails.

| offset | size | field |
|---|---|---|
| 0 | 1 | fragment count |
| 1 | 1 | total zones (64) |
| 2 | 1 | zones per fragment |
| 3 | 1 | flags |
| 4 | 1 | sensor temperature, °C |
| 5 | 1 | zones with a target |
| 6 | 2 | sensor frame number, low 16 bits |
| 8 | 2 | boot count, low 16 bits |
| 10 | 1 | consecutive failed cycles |
| 11 | 1 | firmware version |

Flags: bit 0 sensor fault, bit 1 degraded reading, bit 2 this is a retry.

Boot count and the failed-cycle counter exist because the prototype board has
**no battery sense path at all**. Boot count falls back to 1 whenever the RTC
domain loses power, so frequent cold restarts are the only available proxy for
a dying cell.

### `0x2 HELLO_ACK` — gateway to endpoint

One byte: `0x00` READY, `0x01` BUSY, `0x02` REJECT.

BUSY and silence are deliberately distinct. A half-duplex gateway mid-session with
another endpoint knows it is deaf and can say so; the endpoint then retries in
seconds rather than backing off for minutes. Silence carries no such information
and takes the long backoff.

### `0x3 DATA` — endpoint to gateway

`zones × 3` bytes: `uint16` distance in millimetres, then the raw SNR byte.

Distance `0` means **no target** — the sensor's own sentinel, carried through
rather than translated so that gaps read as gaps and not as a surface at the
sensor. SNR is the raw companded byte; above 40 it is exponentially companded
(AN001096) and is decoded host-side, where the curve can change without a firmware
release.

The grid is sent whole rather than reduced to a level on the endpoint. Feed/wall
classification needs the silo profile and the mount pose, both of which are
per-installation and live in host-side configuration (see ADR-002: with feed at
800 mm in a 5.2 m silo only 8 of 64 zones land on feed at all).

### `0x4 DATA_ACK` — gateway to endpoint

| offset | size | field |
|---|---|---|
| 0 | 2 | received mask, bit *i* set when fragment *i* arrived |
| 2 | 1 | config TLV length, may be 0 |
| 3 | n | config TLV |

**This message is mandatory.** An optional acknowledgement would leave a burst lost
to collision indistinguishable from a delivered one, and the endpoint would sleep
a full window on a silently dropped reading.

The mask is what makes a collision cheap: only the fragments whose bits are clear
go back on air, so losing one fragment costs one ~0.25 s retransmission rather
than the whole ~1.1 s burst.

## Config TLV

Carried inside `DATA_ACK`. Each entry is `tag, length, value…`, except `NOP` which
is a bare padding byte with no length. An unknown tag is skipped, not treated as an
error, so a newer gateway can talk to an older endpoint.

| Tag | Length | Value |
|---|---|---|
| `0x01` | 2 | report interval, seconds (`uint16`) |
| `0x02` | 1 | LoRa channel |
| `0x03` | 1 | sensor frames per reading |
| `0x04` | 0 | reboot |
| `0xFF` | — | no-op padding |

The endpoint validates before applying — a channel outside the ANATEL grants and a
frame count of zero or above the compiled maximum are both rejected with a warning
— and writes to NVS only when a value actually changed.

## Session

```
endpoint                                   gateway
   |  HELLO (seq)                             |
   |----------------------------------------->|
   |                        HELLO_ACK (status) |
   |<-----------------------------------------|
   |   READY: continue.  BUSY: short retry.    |
   |   REJECT or silence: back off.            |
   |                                           |
   |  DATA frag 0 .. 4 (seq)                   |
   |----------------------------------------->|
   |               DATA_ACK (mask, config?)    |
   |<-----------------------------------------|
   |   mask incomplete: resend only the        |
   |   missing fragments, up to 2 more rounds  |
   |                                           |
   |  sleep                                    |
```

`SEQ` is constant for every attempt within one transmission window and increments
once the window closes, delivered or not — so a gap in the sequence is visible to
the gateway. The received mask also persists across attempts, so a retry after a
failed window carries on from what the gateway already has.

### Timers and retry policy

| | Field | Bench |
|---|---|---|
| Transmission window | uniform 1500–2100 s | uniform 18–22 s |
| Retry after silence | uniform 10–120 s | uniform 2–3 s |
| Retry after BUSY | 5 s | 2 s |
| Attempts per window | 4 | 4 |
| HELLO reply timeout | 2 × *P* + 1000 ms | same |
| DATA_ACK timeout | *Q* + *P* + 1500 ms | same |
| Gateway burst quiet timer *Q* | *P* + 750 ms | same |
| Inner data rounds | 3 (1 + 2 retries) | 3 |

*P* is the time one full packet spends on air, `cfg::kPacketAirMs`. The manual does
not give it. It is the ~470 ms measured at 2.4 kbps, scaled by air rate. At 2.4 kbps
the timeouts come out at 1940 ms (HELLO reply), 3190 ms (DATA_ACK) and 1220 ms (*Q*).
The bench window tracks the worst-case session; at 2.4 kbps that is 20 s, which gives
the 18–22 s window above. The endpoint's DATA_ACK timeout must outlast *Q* plus one
packet, or a burst that lost its last fragment is given up before the gateway acks
it. A `static_assert` in `app_config.hpp` enforces this.

Four attempts exhausted, or a retry that would not fit inside the remaining
window, means the endpoint sleeps to the next window instead.

The window deadline is anchored to the *previous* deadline rather than to the
current time, so retry time is absorbed by the window instead of pushing every
later window further out.

**Collision resistance comes from the randomised schedule and the fragment mask,
not from the handshake.** There is no channel reservation and no carrier sense: an
endpoint that wakes mid-session will transmit its HELLO over another endpoint's
DATA. What the handshake buys is learning that the gateway is *busy* — a state it
knows and reports — for ~0.1 s instead of spending the ~1.1 s burst to find out.

The per-node jitter is drawn from a PRNG seeded with the endpoint address. Without
that, `esp_random()` on a node that never brings up Wi-Fi or Bluetooth is not a
true random source, and identical units booting identical firmware could draw
identical jitter, collide every window, and look exactly like a range problem.

## Open items

- Field range at 2.4 kbps. The manual's 5.5 km reference figure assumes clear
  open terrain, a 5 dBi antenna at 2.5 m, and ≥5 V supply for the full 20 dBm. At
  3.2 V from a LiFePO4 cell the module transmits below 20 dBm. A slower air rate is
  not available on these modules (see [Air rate](#radio-layer)), so the remaining
  levers are the supply, the antennas and their height, or a different module.
  Whether firmware `0x82` can be made to accept 0.3k or 1.2k is an open question
  for EBYTE.
- ANATEL dwell-time and duty-cycle rules for digital modulation in these bands.
  Almost certainly moot at a 30-minute cycle; bench retries at 2 s are not.
- Gateway-side arbitration when several endpoints collide repeatedly. v0.1 has no
  mechanism beyond BUSY and the endpoints' own randomisation.
