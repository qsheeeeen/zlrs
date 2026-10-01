# LR2021 key parameters

Source: Semtech LR2021 Final Datasheet Rev. 1.1 (DS.LR2021, 2025-10-14, 236 pages). Only what
ZLRS uses is excerpted here: rates and sensitivity, mode-switch timing, the SPI/IRQ interface
and the pins. How those numbers are used: [07-architecture](07-architecture.md) and
[09-protocol](09-protocol.md); board calibration: the
[board README](../boards/zlrs/rp2350_zero_core2021/README.md).

## Where the part sits

- The first chip of the LoRa Plus family, with the fourth generation of LoRa IP.
- A **half-duplex** transceiver with a single switchless front end and separate LF/HF PA and LNA
  paths.
- 150-2500 MHz: sub-GHz 150-960 MHz on the LF path, S-band and 2.4 GHz ISM on the HF path.
- Backwards compatible with the SX126x / SX127x / SX128x / LR11xx families; FLRC matches the
  SX128x.
- One band at a time, but switching between them is cheap.

## Modulation and rates

- LoRa, (G)FSK, (G)MSK, FLRC, 4-FSK, O-QPSK, OOK, DBPSK (TX only) and LR-FHSS (TX only).
- FLRC runs up to 2.6 Mbps on both sub-GHz and 2.4 GHz.

### FLRC rates and bandwidth (Table 18-1)

| Macro | Raw rate Rb | DSB bandwidth |
| --- | --- | --- |
| `FLRC_BR_2_600_BW_2_6` | 2.6 Mbps | 2666 kHz |
| `FLRC_BR_2_080_BW_2_6` | 2.08 Mbps | 2666 kHz |
| `FLRC_BR_1_300_BW_1_2` | 1.3 Mbps | 1200 kHz |
| `FLRC_BR_1_040_BW_1_2` | 1.04 Mbps | 1200 kHz |
| `FLRC_BR_0_650_BW_0_6` | 0.65 Mbps | 600 kHz |
| `FLRC_BR_0_520_BW_0_6` | 0.52 Mbps | 600 kHz |
| `FLRC_BR_0_325_BW_0_3` | 0.325 Mbps | 300 kHz |
| `FLRC_BR_0_260_BW_0_3` | 0.26 Mbps | 300 kHz |

### FLRC effective rate (Table 18-2, shaped by the FEC coding rate)

| Raw rate | CR=1 | CR=3/4 | CR=2/3 | CR=1/2 |
| --- | --- | --- | --- | --- |
| 2.6 Mbps | 2.6 | 1.95 | 1.73 | 1.3 |
| 2.08 Mbps | 2.08 | 1.56 | 1.38 | 1.04 |
| 1.3 Mbps | 1.3 | 0.975 | 0.86 | 0.65 |
| 1.04 Mbps | 1.04 | 0.78 | 0.69 | 0.52 |
| 0.65 Mbps | 0.65 | 0.488 | 0.433 | 0.325 |
| 0.52 Mbps | 0.52 | 0.39 | 0.34 | 0.26 |
| 0.325 Mbps | 0.325 | 0.244 | 0.216 | 0.163 |
| 0.26 Mbps | 0.26 | 0.195 | 0.173 | 0.13 |

### FLRC sensitivity (1% PER, CR 3/4, rx_boost=7)

| Rate | sub-GHz at 915 MHz | 2.4 GHz |
| --- | --- | --- |
| 2.6 Mbps | -101.5 dBm | -99 dBm |
| 2.08 Mbps | -102.5 dBm | -100 dBm |
| 1.3 Mbps | -104.5 dBm | -101.5 dBm |
| 1.04 Mbps | -105 dBm | -102.5 dBm |
| 0.65 Mbps | -107 dBm | -104 dBm |
| 0.52 Mbps | -108.5 dBm | -105 dBm |
| 0.325 Mbps | -110 dBm | -108 dBm |
| 0.26 Mbps | -111 dBm | -108.5 dBm |

### LoRa sensitivity (64-byte payload, 1% PER, CR 4/5)

| SF | sub-GHz @125 kHz BW | 2.4 GHz @200 kHz BW |
| --- | --- | --- |
| 5 | -113.5 dBm | -117.5 dBm |
| 6 | -116 dBm | -120.5 dBm |
| 7 | -118 dBm | -123.5 dBm |
| 8 | -122 dBm | -126 dBm |
| 9 | -124 dBm | -129 dBm |
| 10 | -127 dBm | -131.5 dBm |
| 11 | -130 dBm | -134.5 dBm |
| 12 | -132.5 dBm | -137.5 dBm |

LoRa symbol rate = `BW / 2^SF`. The datasheet suggests at least 12 preamble symbols for SF5/SF6
and at least 8 otherwise; the firmware's LoRa tier defaults to SF7 / BW125 / CR 4/5 / 8 symbols.

## Mode switch times (Table 3-23, typical)

Defined as the time from the NSS rising edge that ends the mode-switch SPI transaction to BUSY
going low.

| Symbol | Description | typ |
| --- | --- | --- |
| `TSHFOSC` | HF crystal wake-up (within 10 ppm) | 700 us |
| `PLL_LOCK` | synthesiser lock | 32 us |
| `TSPDRCS` | Sleep to STDBY_RC, cold start | 6 ms |
| `TSPDRCW` | Sleep to STDBY_RC, warm start (context kept) | 1 ms |
| `TSRCXO` | STDBY_RC to STDBY_XOSC | 43 us |
| `TSRCFS` | STDBY_RC to FS | 76 us |
| `TSRCRX` | STDBY_RC to Rx | 115 us |
| `TSRCTX` | STDBY_RC to Tx | 106 us |
| `TSXOFS` | STDBY_XOSC to FS | 44 us |
| `TSXORX` | STDBY_XOSC to Rx | 84 us |
| `TSXOTX` | STDBY_XOSC to Tx | 76 us |
| `TSFSRX1` | FS to Rx (LoRa) | 53 us |
| `TSFSRX2` | FS to Rx (FSK/FLRC) | 37 us |
| `TSFSTX1` | FS to Tx | 45 us |
| `TSRXFS1/2` | Rx to FS | 10 / 11 us |
| `TSTXFS` | Tx to FS | 54 us |
| `TSTXRX` | Tx to Rx | 91 us |
| `TSRXTX` | Rx to Tx | 107 us |

How the superframe uses these numbers: [07-architecture](07-architecture.md) 4.

## SPI and the host interface

- Two kinds of command: **normal commands** (raise BUSY and have to be waited for) and **direct
  read/write** (executed at once, no BUSY). Low-latency FIFO access uses the latter.
- BUSY goes high on the NSS falling edge and low once the command is done.
- `SetPacketType` only works in Standby RC, Standby XOSC or FS mode.
- Command space: `0x01xx` for system configuration, `0x02xx` for radio configuration.

### Commands that matter for scheduling

| Command | Opcode | Purpose |
| --- | --- | --- |
| `SetRfFrequency` | 0x0200 | set the carrier frequency (Hz, 32-bit) |
| `SetRxPath` | 0x0201 | pick the LF/HF receive path and rx_boost |
| `SetStandby` | 0x0128 | standby on RC or XOSC |
| `SetFs` | 0x0129 | enter frequency synthesis |
| `SetRx` / `SetTx` | - | start receive or transmit |
| `SetRxTxFallbackMode` | - | where to land after a transfer |
| `SetAutoRxTx` | 0x0211 | Tx switches to Rx after Delay, Timeout bounds the stay |
| `SetRxDutyCycle` | 0x0210 | periodic Rx windows with sleep, can work with CAD |
| `SetDefaultRxTxTimeout` | 0x0215 | default transfer timeout |
| `CalibFE` | 0x0123 | calibrate the front end per LF/HF path (4 MHz steps) |
| `SetDioIrqConfig` | 0x0118 | map IRQs onto DIO pins |
| `GetAndClearIrqStatus` | 0x0117 | read and clear interrupt status |

### IRQ bits (excerpt of Table 5-17)

| Bit | Name | Meaning |
| --- | --- | --- |
| 5 | `PreambleDetected` | preamble detected |
| 7 | `CadDetected` | channel activity detected |
| 18 | `RxDone` | receive finished |
| 19 | `TxDone` | transmit finished |
| 20 | `CadDone` | CAD finished |
| 21 | `Timeout` | transfer timed out |
| 22 | `CrcError` | CRC error |
| 23 | `LenError` | length error |
| 24 | `AddrError` | address mismatch |
| 2 | `RngReqVld` | a valid ranging request arrived |
| 16 | `Error` | a non-command error |
| 17 | `CmdError` | a host command failed |

## Pin notes (QFN32)

- `RFO_LF1` / `RFO_LF2`: LF PA outputs (sub-GHz)
- `RFI_LF`: LF receive input
- `RFI_HF`: HF receive input
- `RFO_HF`: HF PA output (2.4 GHz)
- `VTCXO`: can power an external TCXO, with the voltage set by `SetTcxoMode`

The switchless front end needs matching networks for LF and HF to coexist. The Core2021 module
used here carries them, and it uses a crystal rather than a TCXO (`tcxo-*` stays off, `reg-mode`
uses DC/DC); see the [board README](../boards/zlrs/rp2350_zero_core2021/README.md).
