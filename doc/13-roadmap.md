# Roadmap

This document holds **to-dos, the pending-defaults ledger and acceptance criteria** only.
Finished work gets a one-line summary; details live in code comments and the design is in the
design documents.

## Implemented behavior

- **Runtime data24 mode**: management setting `set-mode data24` disables control and uses the
  2.4 GHz data link for pairing, session setup and bidirectional serial transfer. Switching back
  to mixed mode preserves the pairing record. Both LF prototype boards were cleared, configured,
  and paired from scratch in data24; one paced 4096-byte transfer in each direction completed
  byte-exact in 30.95 s (32 bytes / 50 ms input pacing). Reusing all four timer windows and
  reducing the data24 preparation reserve to 5 ms increased the planned MTU to 246 bytes. A
  paced 4 KiB bidirectional LF test completed in 9.23 s (32 bytes / 50 ms), versus 30.95 s before
  reusing all timer windows. A 32 KiB bidirectional test completed byte-exact in 55.52 s (64
  bytes / 100 ms), with no USB input drops; 16 KiB at 56 bytes / 80 ms completed in 25.12 s.
  Raising the pace to 60 bytes / 80 ms overflowed the input rings and stalled delivery. This
  exposes a serial-to-radio backpressure/ARQ limit; it is not maximum radio goodput. HF has build
  coverage, with hardware validation pending.

## To-do

### Radio and protocol

- **Hardware timer and driver ownership (implemented; short LF checks passed)**: RP2350 TIMER1
  schedules absolute slots; one application thread owns the independent USP RAL/LR20xx driver,
  without RAC/planner initialization or a USP worker. Delayed slots are skipped with hop/frame
  accounting, and RX follows authenticated IRQ timestamps. Calibration tables now have the
  three entries the BSP consumes, aborted TX packets cannot leak FIFO bytes into the next TX,
  and authenticated SYNC/CFM carry a 32-bit frame counter. Both peers must upgrade together.
  That baseline used a 3000 us preparation guard; CN470 LF was clamped to 45 Hz (22222 us frames),
  rather than advertising an unsustainable 150 Hz schedule. Logs queue in RAM during RF and
  flush at boot or management pauses, keeping flash writes out of normal radio slots.
  On 2026-10-01 two LF boards, with both links, hopping, P1, handshake, telemetry and ARQ enabled,
  passed three consecutive simultaneous 4096-byte transfers per direction in 6.72, 6.97 and
  6.87 s, byte-exact. Wireless checks used the debug image. LF and HF production images build;
  both LF devices were subsequently updated to the production image with running hash verified
  and confirmed. Production wireless transfer and HF hardware acceptance remain pending. The host
  suite passes 2025 checks under ASan/UBSan, including frequency-aware two-node simulation,
  skipped slots, timer wrap, different boot epochs and silent data resynchronization.
  Ten additional consecutive duplex rounds also passed, in 6.56-6.94 s each. Input was paced
  at 32 bytes per 50 ms, so these times do not measure maximum RF goodput. Over a 148 s steady
  observation, neither board added a missed timer boundary or a control handshake; RX added no
  late launch and TX discarded one late launch. A separate RX radio pause of 6.036 s retained
  a 4096-byte TX-to-RX transfer, byte-exact, completing in 9.043 s total without reboot or
  re-pairing. Management intentionally discards new bytes arriving on the paused device's local
  data input; this check validates the transmitting peer's retained data instead.
  Long-duration timing and higher-rate acceptance remain open; these are short LF checks.
- **Packet preparation and FIFO preloading (implemented; short LF checks passed)**: construction,
  P1 authentication, modem/frequency setup, TX FIFO loading and FS entry happen before the RF slot.
  A dedicated timer meta-IRQ bottom half issues only the prepared TX command; the link thread
  waits for its result and keeps exclusive ownership of preparation and completion. The default
  reservation is 2800 us with a 200 us launch offset, retaining CN470 LF at 45 Hz. USB core and
  controller work was made preemptible during the initial checks; carried Zephyr patches fixed
  the final CDC ZLP completion race exposed by that scheduling. These vendor changes are now
  removed: USB/CDC use upstream scheduling, and the application timer bottom half provides TX
  priority. Data/CRSF callbacks also recheck an empty TX ring before disabling
  its notification, preventing a preempting producer's wakeup from being lost.
  On 2026-10-01 two LF boards running the final debug image passed 20 consecutive simultaneous
  4096-byte transfers per direction, byte-exact, in 1.71-5.17 s each, with input paced at
  32 bytes per 10 ms. A 6.031 s RX radio pause retained a separate 4096-byte TX-to-RX transfer,
  byte-exact, completing in 7.835 s total. Host tests pass 2033 checks under ASan/UBSan, and LF/HF
  production builds pass. Both debug images were verified by running hash and confirmed.
  FIFO readiness snapshots leave about 0.6-1.2 ms before launch. Occasional late launches are
  still discarded; these checks do not accept worst-case launch jitter or maximum RF goodput.
  A 2000 us reservation at 52 Hz passed short transfers but showed late launches and control
  reacquisition before the final timer bottom-half change; it is not the default and needs a new
  stability check. Long-duration timing, production wireless acceptance and HF hardware checks
  remain pending.
  After removing all Zephyr and USP HAL patches on 2026-10-01, both LF devices passed another
  20 consecutive duplex 4096-byte rounds at 32 bytes per 10 ms, byte-exact. A 6.022 s RX pause
  retained a separate 4096-byte TX-to-RX transfer, completing byte-exact in 9.027 s. LF/HF
  production configurations build against clean upstream modules. Both LF debug images were
  hash-verified and confirmed. The upstream HAL's existing 1 s BUSY timeout configuration remains;
  a timeout raises `k_oops` and resets the board instead of returning a recoverable radio error.
  The application timer bottom half remains; USB/CDC keep upstream scheduling and no patches
  are applied during setup. These checks retain the same pending timing and board acceptance.
- **Direct platform integration (implemented; short LF checks passed)**: removed `app/src/ports/`,
  the radio ops/submit adapter and the separate management transport files. CRSF/data bind
  directly to Zephyr UART/CDC devices, management owns its CDC in `mgmt/mode.c`, and the LR2021
  driver lives in `platform/radio.c`. Hardware timer and FIFO preloading remain in use.
  On 2026-10-01 LF debug and LF/HF production builds passed; host tests passed 2033 checks
  under ASan/UBSan. Both LF debug images were hash-verified and confirmed. The first duplex
  check after sequential upgrades timed out: control was connected and USB input was queued,
  but TX data stayed SEARCHING with authentication rejects. After rebooting both boards,
  three consecutive duplex 4096-byte rounds passed byte-exact in 2.86-4.22 s with input at
  32 bytes per 10 ms. A 6.043 s RX management pause retained a separate 4096-byte transfer,
  completing byte-exact in 8.799 s. Recovery after sequential peer upgrades needs investigation;
  its cause is not established by this check. Production wireless and HF hardware checks remain
  pending. Upstream Zephyr/USP source is unchanged.
- **433 MHz prototype configuration (implemented; short LF check passed)**: LF devicetree
  now defaults to CN433 instead of CN470. On 2026-10-01 both attached boards had their saved
  region set to CN433 and were cold-rebooted; status and live console frequencies confirmed
  433 MHz control operation with active sessions. The existing debug firmware uses the saved
  configuration; the updated LF production image builds but was not installed for this check.
  Three consecutive duplex 4096-byte rounds passed byte-exact at 32 bytes per 10 ms.
  This does not replace antenna/PA characterization or long-duration timing acceptance.
- **Peripheral DMA configuration (enabled; wireless performance regressed)**: application SPI0
  uses upstream PL022 DMA on TX/RX channels 0/1; the PIO status LED uses channel 2. LF debug
  and complete LF/HF production sysbuilds pass. Both attached LF debug images were hash-verified
  and confirmed on 2026-10-01. No upstream code was changed. UART0/1 hardware has DMA request
  sources but the current PL011 driver implements neither DMA nor async transfers; USB UDC,
  flash and TRNG have no configuration path to enable system DMA. Unused peripherals remain off.
  The old 2800 us preparation reservation failed with SPI DMA. The DMA default is now 6000 us,
  with an 800 us startup budget and a 500 us receiver phase estimate, reducing CN433 to 27 Hz.
  SET_TX command calls measured about 190-217 us in snapshots, higher than the polling path.
  One duplex 4096-byte round at 32 bytes per 10 ms passed in 3.06 s, but the following round
  timed out at 45 s: TX-to-RX received 4000/4096 bytes, RX-to-TX was exact. At 32 bytes per
  50 ms, three consecutive duplex rounds were byte-exact in 22.95, 10.72 and 17.63 s.
  A 6.036 s RX management pause test received only 1796/4096 bytes within 36.043 s total and
  did not pass. These results establish functioning DMA transfers, not throughput or pause
  recovery acceptance. Excess retransmissions and the performance regression need investigation;
  IRQ/launch worst-case timing, production wireless and HF hardware acceptance remain pending.
- **First frames in the air, one board (passed)**: with the Core2021 attached, pairing
  `PAIR_REQ` frames really go out: one every 100 ms through the 30 s window, about 1700
  transactions over five rounds with `fail=0`. The CRSF entry was checked on the same board:
  911 valid RC frames fed into the CRSF CDC gave `crsf_bad=0`, then 550 frames with the wrong
  payload length took `crsf_bad` up by exactly 550. SPI, the control lines, RAC transactions and
  CRSF parsing are covered; receive is not part of it.
- **Pairing, sessions and round trips on both links**: on 2026-09-28 two LF boards completed a
  fresh pairing and restored their records after an update; the isolated 2.4 GHz data link
  completed its session handshake.
  After ARQ recovery and overrun pacing fixes, fixed-channel 2441 MHz passed three consecutive
  simultaneous transfers of 4096 bytes per direction, byte-exact, in 23.829, 21.978 and 25.487 s.
  Both boards run the data-only profile with authentication, session handshake and ARQ enabled.
  A further simultaneous 512-byte transfer per direction survived a six-second RX radio pause
  in management mode and completed byte-exact in 13.746 s, without re-pairing or rebooting.
  These are historical RAC-backend results. Throughput remained low with substantial retransmissions. Before
  those fixes both 64-byte transfers with hopping enabled received zero bytes; the timer-backend
  short hopping checks above now pass. Handshake success alone is not acceptance.
- **Sub-band mixing on hardware**: the software side is done (two hop tables, `SYNC` carrying
  the mix and the table version, the telemetry-then-data-then-RC migration order, the regional
  duty cap and LQ-driven adaptation) and the host tests cover both ends picking the same
  frequency and falling back; the real switching cost and interference benefit wait for a second
  module.
- **Higher control rates, including 1000 Hz** (`r=0`, all 2.4G): hardware timing and exclusive
  driver ownership are in place. Reduce measured preparation and band-switch cost, then optimize
  packet format and slot allocation. The current four equal slots and preparation guard cannot
  sustain 1000 Hz. Each new rate needs measured latency, loss and jitter acceptance.
- **LoRa long-range tier on hardware**: sensitivity, packet error rate curves and the gap
  between measured airtime and the model. EU868's 1 % duty cannot feed this tier, so it is
  offered only in regions without a duty cap.
- **Reliable transfer (ARQ) on hardware**: on by default (a four-frame window, cumulative ACKs,
  a retransmit timeout of at least 30 ms and two actual data frame periods, 64 retries).
  Recovery now answers pending ACKs before probing and retains
  receive sequence/reassembly progress. Regression tests cover simultaneous exhaustion,
  acknowledged complete/partial prefixes and lost ACKs without duplicate delivery; the host
  suite includes these regressions under ASan/UBSan. Both peers must upgrade together because older
  firmware discards receive progress on a recovery probe. Fixed-channel duplex passes the
  short hardware test above; long-duration throughput and latency acceptance remain open.
  Later the ack can shrink to one header bit once the RX always has a frame to ride along.
- **Over-the-air firmware update** (optional): images travel over the 2.4G data link, roughly
  10 s per MB.

### Hardware

- **Carrier board spin**: the two-layer board from [05-carrier](05-carrier.md).
- **Single-port variant**: no USB, one UART port whose link role is configured at runtime
  ([03-platform](03-platform.md) 2.1). This needs firmware support that does not exist yet: the
  `zlrs-link-*` aliases and `zlrs,default-role` property, the `ZLRS_PORT_LINK*` capability
  macros, a port layer that serves either link, and `set-port-role` in the management protocol.
- **RP2350 TRNG** (verified on hardware, on by default): Zephyr's `entropy_rpi_pico` is unusable
  (pico-sdk reads with interrupts off and busy waits, which stops USB dead; reproduced on the
  board). `drivers/entropy/` is ZLRS's own driver: probe soft-resets, configures and
  pre-fetches, reads come from a two-block pool, **interrupts stay on and no spinlock is
  taken**, and a timeout or failed check is an error rather than something to paper over.
  Register timing follows datasheet 12.12 and the bootrom; the sample count is 100 because the
  datasheet's suggested 20-25 fails the internal Von Neumann check on RP2350 (measured: runs of
  32 zeroes or ones), while ten blocks at 100 came out clean. Verified on the board: entropy
  self-test ok at boot, 25 consecutive `zlrs entropy` runs and over 150 s of uptime without a
  watchdog reset.

### Update path

- **MCUboot slot swap plus host confirmation** (verified on hardware, see
  [08-firmware](08-firmware.md) 5): all four paths pass: the first image needs no confirmation,
  an upgrade plus `confirm` stays, no confirmation means a reset and rollback after 20/60 s, and
  a hung application is rolled back by the bootloader's 30 s watchdog. Two details: the swap has
  to use `swap using move` (`swap using offset` goes in but never comes back on this board) and
  the tool has to name the inactive slot, since MCUboot's recovery upload defaults to the
  primary one.
- **Terminal management and update workflow**: one `tools/zlrs.py` entry point for management,
  recovery, updates and throughput; grouped status output, expanded help, image preflight,
  USB serial tracking, live upload progress and explicit confirmation results are implemented.
  Simulated CLI regressions cover errors and confirmation ordering. On 2026-10-01 two LF devices
  passed discovery, queries, management session entry/exit and a selected RX same-image re-upload
  through recovery, reboot, hash verification and confirmation; IDs and pairing were retained.
  WSL USB re-enumeration required usbipd auto-attach. Human and JSON throughput commands correctly
  reported failed delivery: 0/512 bytes per direction before update and 0/64 afterward, both timed
  out with hopping and both links enabled. This does not accept wireless data delivery.
  Initial installation, cross-version
  upgrades and unconfirmed rollback through the new entry point remain pending.
- **mcumgr in the application** (optional): update while running, writing the image straight
  into slot1.

### Security

- **P2 encryption profile**: implement once everything else is done (AES-CTR with the MAC over
  the ciphertext).

## Pending defaults

Some knobs are on or off today only because they have not been accepted on hardware yet — "what
the system will do" is already decided, "proven on a board" is not. Each row is that debt: the
knob, its default today, what has to be measured, and what happens afterwards. This is the only
place where "not tested yet" is written down; [12-debug](12-debug.md) owns the switches
themselves.

| Knob | Default today | What is missing | Afterwards |
| --- | --- | --- | --- |
| `ZLRS_APP_RATE_ADAPT` | off | per-tier packet error rate and SNR thresholds, measured with a second module | on, thresholds replaced (the placeholders in `app/src/link/rate.c` are ignored today) |
| `ZLRS_APP_WATCHDOG` | on | a long run on hardware without false resets (PBKDF2 inside management mode is the slowest feed interval) | stays on |
| `ZLRS_APP_DATA_ARQ` | on | throughput and latency on hardware; the ack can shrink to one header bit once the RX always has a frame to ride along | stays on, window sized from measurements |
| `ZLRS_APP_DYNAMIC_POWER` | on | thresholds and hysteresis from measured RSSI/SNR | stays on, thresholds replaced |
| `ZLRS_APP_SUB_BAND_MIX` | on | switching cost and interference benefit, with two modules | stays on |
| `ZLRS_APP_FHSS`, `ZLRS_APP_SESSION_HANDSHAKE`, `ZLRS_APP_TELEMETRY` | on | the first two-module session (pairing, session, telemetry round trip) | stay on |
| rate tier and frame-rate values, telemetry period | compile time | management-protocol commands to match [08-firmware](08-firmware.md) 3 | runtime, stored in NVS (role and security profile already are) |

The switches that cut the product's *shape* (which links, which band role, which modulation) are
not debt: they stay and any image may set them. The ones that only exist to isolate a layer on
the bench — P0, hopping off, session handshake off, telemetry off, ARQ off, sub-band mixing off,
radio off — are gated by `CONFIG_ZLRS_APP_DEBUG`, so a shipped configuration cannot reach them;
see [12-debug](12-debug.md) 3.

## Acceptance criteria

| Item | What counts as passing |
| --- | --- |
| Hardware freeze | the module's RSSI for a known transmitter on the target band matches the datasheet's order of magnitude |
| Pairing and sessions | two devices pair on a long press, establish a session and run both the control and data links |
| Configuration | a blank RX can be configured with the tool (management CDC) until it is able to pair |
| Data link | short the RX's UART TX to RX as a loopback, send data from the TX side and get it back |
| 1000 Hz | steady 1000 Hz in the all-2.4G shape, with latency and loss that can be measured |
| Interference | link retention and recovery time measured with several devices on the band plus interferers |
| Single-port board | one UART, role switched by configuration, both roles verified on hardware |
