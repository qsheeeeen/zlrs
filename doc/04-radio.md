# Radio requirements

ZLRS uses one Semtech LR2021 in half-duplex FLRC mode. SubG and 2.4 GHz share the
radio and cannot carry simultaneous DATA traffic. Module coverage, PA tables and
antenna paths are hardware constraints; the selected band supplies its modem and
channel plan.

Chip framing and electrical limits are referenced to the
[Semtech LR2021 datasheet, revision 1.1](https://www.mouser.com/pdfDocs/61979758LR2021_V1_1_datasheet.pdf).
The chip supports FLRC up to 2.6 Mbps and variable-length FLRC payloads of 6–511
bytes. Datasheet transition times are typical chip values, not the complete host
preparation budget.

## DATA modem profile

| Parameter | Requirement |
| --- | --- |
| Modulation | FLRC |
| Coding rate | 3/4 |
| Pulse shaping | BT 0.5 |
| Preamble | 24 bits |
| Hardware synchronization word | 32 bits, full-word matching |
| Physical CRC | 16 bits |
| Packet length | Variable, at most 511 software-frame bytes |
| DATA sync word | Four bytes derived from pairing material; hardware slot 1 |
| Pairing sync word | ASCII `ZLRS`; hardware slot 2 |

The operational raw-rate tiers are 2.6, 1.3, 0.65 and 0.325 Mbps. The default
2.4 GHz rate is 2.6 Mbps. The SubG rate is limited by the selected regional policy.
Physical coding, headers and control traffic reduce delivered user throughput;
raw bitrate is not serial throughput.

| Region | Module SKU | Configured band limits | Raw-rate ceiling | TX ceiling | Duty policy |
| --- | --- | --- | --- | --- | --- |
| FCC915 | HF | 902–928 MHz | 2.6 Mbps | 22 dBm | No software duty cap |
| EU868 | HF | 863–870 MHz | 0.325 Mbps | 14 dBm | 1% over 60 s |
| CN470 | LF | 470–510 MHz | 0.325 Mbps | 17 dBm | No software duty cap |
| CN433 | LF | 433.05–434.79 MHz | 0.325 Mbps | 10 dBm | No software duty cap |

These are ZLRS operating policies, not a certification claim. The region must fit
the board's declared SubG coverage. Each board declares minimum and maximum
transmit setpoints for both paths. The prototype range is −17 to +12 dBm on
2.4 GHz and −10 to +22 dBm on SubG. The effective SubG maximum is the smaller of
the board maximum and regional ceiling; the SubG region does not constrain the
2.4 GHz setpoint. Antenna gain and measured output require qualification on the
finished hardware. Power policy is specified in [firmware](06-firmware.md#automatic-band-operation-and-power).

## Transfer and timing contract

Commands and FIFO transfers use synchronous polling SPI. The RF owner prepares
modulation, frequency, power and packet data before the fixed transmit deadline.
A hardware timer launches the prepared SET_TX command 200 µs after the slot
boundary. Ordinary SPI preparation and unbounded BUSY waits shall not run in IRQ
context. Preparation failure cancels that transmission; late transmission is not
allowed.

The preparation reservation is 2.5 ms on 2.4 GHz and 6 ms on SubG. Slot planning
also reserves 500 µs for startup, the maximum 511-byte packet airtime and 200 µs
for completion margin. The radio's airtime calculation must use the same physical
settings as the actual modem. Short packets keep the same slot boundaries and
consume only their actual airtime.

A DATA frame may exceed the initially loaded FIFO prefix. Up to 128 bytes are
loaded before launch; the remaining bytes are supplied promptly after launch.
Tail loading must finish before FIFO underflow. Receive lengths and FIFO tails
must be checked against capacity before subtraction or copying. Underflow,
transfer failure and missing completion IRQs must produce a failed transaction
and cleanup, rather than a successful delivery indication.

Radio IRQs provide hardware timestamps; the RF owner performs protocol completion
and phase correction. Critical launch, timestamp and RF processing paths run from
SRAM. Retuning reserves a shared 12 ms frame-boundary interval as specified in the
[air protocol](07-protocol.md#automatic-band-switching).

## Calibration and observation

Core2021 uses a crystal, DC/DC supply mode and the internal low-frequency RC
clock. Receiver boost is 7. SubG PA/current tables differ between LF and HF;
2.4 GHz uses the shared high-frequency path. Reference tables require module and
carrier calibration. The [board reference](../boards/zlrs/rp2350_zero_core2021/README.md)
specifies calibration frequencies.

Management status provides junction and XOSC-area temperatures, VBAT, version,
mode, command/reset status, IRQs and chip errors. Reads run with RF paused and
have independent validity flags. They describe the chip at the maintenance
boundary; they do not report ambient temperature or calibrated battery voltage.
FLRC does not provide LoRa SNR. See [management status](07-protocol.md#management-status)
and [acceptance](10-debug.md).
