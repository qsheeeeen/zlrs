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
- **USB input backpressure (implemented; LF data24 hardware checked 2026-10-02)**: a full
  application RX ring now pauses the CDC callback until the radio owner frees space. The
  upstream CDC FIFO then throttles USB OUT; no upstream source changes are required. Drop
  counters count bytes, and `zlrs ports` reports queued bytes and RX pause state. Previously an
  unpaced 16 KiB duplex write delivered only 8684/8192 bytes before timing out, with application
  input overflow. Both LF boards now pass three consecutive unpaced 16 KiB duplex rounds,
  byte-exact, in 4.863, 3.166 and 3.166 s, using 246-byte USB writes and P1 at -10 dBm.
  All 49152 input/output bytes per device were accounted for, with zero USB input or host output
  drops. This supersedes the paced-input overflow finding above; it does not accept radio
  throughput. The schedule still reserves 5000 us per 7352 us slot (246-byte MTU), and the
  three rounds added 309/326 retransmissions for about 204 new DATA frames per sender.
- **Synchronous management entry (implemented; both LF boards checked)**: `ENTER` ACK follows
  session setup, queued log processing and file-backend synchronization. Repeated ENTER also
  synchronizes logs. Pipelined and split commands, immediate STATUS/log reads and MCUboot
  upload/verification/confirmation passed on both boards. No logging thread runs during RF.
- **Receive-window recovery (implemented; LF data24 hardware checked 2026-10-02)**: header,
  CRC and length errors restart single-packet RX within the original slot deadline. FIFO and
  timestamp activity no longer completes a pending radio request. Both LF boards passed three
  unpaced 16 KiB duplex rounds, byte-exact, in 3.861, 3.360 and 3.559 s, with zero serial drops
  and no additional missed timer boundaries. Error-triggered restarts occurred on both boards;
  this check shows no clear throughput gain over the preceding 3.812, 3.513 and 2.560 s rounds.
  Retransmissions and the 5 ms slot preparation reserve remain throughput limits. IRQ service
  diagnostics include reading and clearing status over SPI, not just thread wakeup latency.
- **Duplicate ACK timeout (implemented; LF data24 hardware checked 2026-10-02)**: ACKs that
  do not advance the send window no longer reset the lost head's retransmission timer. The
  regression reproduces the delay with continuous reverse data, including sequence wrap;
  all 2100 host checks pass under ASan/UBSan. Both LF boards passed three unpaced 16 KiB duplex
  rounds, byte-exact, in 2.308, 2.306 and 2.457 s, versus 3.861, 3.360 and 3.559 s before this
  change. Neither device dropped serial bytes or added missed timer boundaries; retransmissions
  fell from 326/312 to 238/242 across the three rounds. The receiver still discards frames beyond
  a gap, and timer/schedule throughput acceptance remains pending.
- **ARQ receive window (implemented; LF data24 hardware checked 2026-10-02)**: authenticated
  frames beyond a gap are retained inside the four-frame window and delivered in order when
  the missing head arrives. The frame format and cumulative ACK stay unchanged. All 2170 host
  checks pass under ASan/UBSan, including multiple losses, sequence wrap, duplicates, window
  bounds, recovery and session reset. Both LF boards passed three unpaced 16 KiB duplex rounds
  in 1.512, 1.360 and 1.762 s, and three 64 KiB duplex rounds in 4.711, 5.015 and 5.019 s,
  byte-exact. No serial drops or new missed timer boundaries occurred; the longer rounds delivered
  13.1-14.4 kB/s per direction. Retransmissions in the 16 KiB rounds fell from 238/242 to 89/78.
  This approaches the current schedule's roughly 16.7 kB/s per-direction ceiling, not the FLRC
  physical limit. Slot preparation still reserves 5 ms; late launches and radio errors remain.
- **Data24 slot budgeting and preparation boundaries (implemented; LF checks 2026-10-02)**:
  data24 sizes each slot from the maximum 255-byte frame, its own preparation reservation,
  launch/startup allowances and separate deadline/completion margins. Preparation now waits
  for its hardware-timer boundary, preventing an idle predecessor from opening RX in an
  earlier slot. The default remains 5 ms: 7290 us per slot and 14580 us per TX/RX pair. Both
  LF boards passed five consecutive unpaced 64 KiB duplex rounds, byte-exact, in 5.061, 4.961,
  5.013, 5.212 and 4.610 s, with no serial drops, recovery probes or new missed timer boundaries.
  LF/HF production applications build. Before the preparation-boundary fix, 3/4 ms reservations
  increased late attempts and gave no throughput gain; those reservations are not accepted.
  The final five-round check added 15 preparation delays per board, versus 69/49 delayed trigger
  attempts. IRQ-to-link-completion and protocol-call timings are available in `zlrs radio`;
  their lifetime maxima include startup/session work. Trigger latency, completion SPI and
  authentication still need optimization before shrinking the slots further.

- **RP2350 hardware protocol crypto (implemented; both LF boards checked 2026-10-02)**:
  the public Pico SDK SHA-256 accelerator replaces the PSA protocol backend, preserving
  HMAC/HKDF/PBKDF2 bytes. Both boards pass 30 independent known-answer checks, including
  complete SHA/HMAC digests, unaligned and split input, padding boundaries, long keys,
  42-byte HKDF output and multiple PBKDF2 iterations. Full-frame tags take about 79 us
  without RF-thread interference; shell measurements during RF average 101-108 us and include
  preemption. LF/HF production applications build; all 2170 host checks pass (software crypto,
  not the hardware backend). At the unchanged 5 ms reservation, five unpaced 64 KiB duplex
  rounds pass byte-exact in 5.010, 4.916, 4.566, 4.669 and 5.015 s, with zero serial drops,
  recovery probes or new missed timer boundaries. Preparation delays during those rounds fall
  from 15/15 to 0/0; trigger delays remain 51/31. This accepts the backend, not higher maximum
  throughput. A one-sided update against the preceding PSA image timed out with session/phase
  recovery trouble; the second update restored delivery. Single-reboot recovery needs diagnosis.
- **Split timer diagnostics (implemented; LF 3 ms experiment 2026-10-02)**: alarm entry and
  thread resumption are measured separately for preparation and launch; the last rejected
  preparation records entry delay, build time and SPI time. With hardware crypto, five 3 ms
  64 KiB duplex rounds pass in 7.825, 6.066, 6.016, 6.018 and 5.767 s, slower than 5 ms.
  Three additional diagnostic rounds pass in 6.163, 6.213 and 5.963 s. Typical last rejected
  preparations entered 1506/1738 us late, built in 380/382 us and used 1529/1364 us in SPI.
  Launch alarm-entry maxima are 61/114 us, versus 123/209 us from alarm entry to thread
  resumption. Accounting now runs after the launch command to reduce measurement overhead.
  The default stays 5 ms; preparation scheduling, launch notification and SPI remain open.

- **Idle clock retention (implemented; both LF boards checked 2026-10-02)**: acquired RX
  retains its phase for at least two configured SYNC intervals before scanning. The old
  unconditional 300 ms threshold scanned during a legal idle gap: at the 5 ms data24
  reservation, SYNC arrives every 3.718 s. A first direct-timer experiment consequently
  timed out on the first stream with zero delivery and a receiver phase error over 4 ms.
  The cadence-derived guard is 7.451 s at that reservation; initial acquisition keeps the
  300 ms guard. Seven new host checks cover acquisition, rounding, cadence and overflow;
  all 2177 checks pass with ASan/UBSan. With the direct launch and RF scheduling changes
  below, five unpaced 64 KiB duplex rounds pass byte-exact after an idle interval.

- **Direct timed TX and RF servicing (implemented; both LF boards checked 2026-10-02)**:
  TIMER1 alarm 3 issues only the prepared five-byte `SET_TX` in a RAM-resident zero-latency
  ISR; it uses a SRAM mailbox and bounded polling without kernel calls. An ordinary alarm
  then notifies the owner. All other SPI operations retain DMA. Data24 raises its RF owner
  to meta-IRQ priority; serial TX/RX notifications run on the system workqueue, and management
  restores ordinary priority. No upstream source is modified. LF/HF production builds pass.
  With the idle phase guard, five unpaced 64 KiB duplex rounds pass byte-exact in 4.309, 4.214,
  4.309, 4.211 and 4.261 s (15.2-15.8 kB/s per direction). Both boards add zero preparation or
  trigger delays, serial drops, recovery probes or missed timer boundaries during the rounds.
  Trigger jitter is at most 3 us; `SET_TX` takes 4 us, with observed startup maxima of 269/243 us.
  Retransmissions add 97/88. This verifies trigger stability at the unchanged 5 ms reservation;
  it does not establish the physical throughput limit or tighter-slot acceptance.

- **RP2350 Semtech HAL (implemented; both LF boards checked 2026-10-02)**: ZLRS supplies
  the documented public HAL, retaining the upstream board/BSP and radio driver. CMake selects
  the implementation without editing upstream files. Commands of at most 32 bytes use bounded
  polling; larger transfers use contiguous SPI DMA buffers, avoiding scatter-buffer DMA restarts.
  Reads explicitly send NOPs, fixing the upstream FIFO-read echo of stale buffer contents onto
  MOSI. LF/HF production builds pass. At 3 ms preparation, five unpaced 64 KiB duplex rounds
  pass byte-exact in 3.057, 3.162, 3.108, 3.111 and 3.063 s; observed useful rates are
  20.7-22.6 kB/s per direction. Neither board adds late preparation/trigger attempts, missed
  boundaries, recovery probes or serial drops. Trigger jitter is at most 4/5 us; typical setup
  falls from about 1 ms to 0.3 ms. Retransmissions add 86/18. The data24 default becomes 3 ms;
  tighter reservations and the physical throughput limit remain pending.

- **FIFO DMA ownership (implemented; both LF boards checked 2026-10-02)**: the RP2350 HAL
  allocates its own two channels through the public DMA controller API, leaving the generic
  SPI driver's channels alone. Descriptors and buffers are static; only RX completion wakes
  the RF owner. This removes generic SPI scatter/context bookkeeping. The RP2350 controller's
  `dma_reload()` currently starts the channel, so the HAL uses explicit `dma_config()` and
  `dma_start()` rather than relying on a stopped reload. At 1 ms without SRAM relocation,
  five 64 KiB duplex rounds remain byte-exact but take 11.2-13.8 s with frequent late attempts;
  that reservation is not accepted. The combined SRAM validation below has no late attempts.

- **SRAM RF hot path and early preparation (implemented; both LF boards checked 2026-10-02)**:
  Zephyr relocates the RF application and portable protocol text to SRAM without changing core
  sources or the MCUboot update path. Connected data24 builds its next TX immediately after
  RX completion; RX entry and acquisition retain their boundaries. The constant IRQ mask is
  configured once, completed packets use hardware FS fallback, and BUSY is checked without a
  redundant status read. At 1.5 ms preparation and 500 us startup budget, five unpaced 64 KiB
  duplex rounds pass byte-exact in 2.061, 2.053, 2.057, 2.004 and 2.058 s (31.8-33.5 kB/s per
  direction). The same 1.5 ms schedule without SRAM took 3.062-3.265 s and added late attempts.
  With SRAM, neither board adds preparation/trigger delays, missed boundaries, recovery probes
  or serial drops; trigger jitter is at most 3/2 us. Debug text relocation adds about 27 KiB
  of SRAM. The default becomes 1500 us, giving 3490 us slots and 6980 us pairs. LF/HF production
  builds pass. Tighter slots and larger FLRC packets still need hardware acceptance.

- **1 ms data24 reservation (accepted with SRAM; both LF boards checked 2026-10-02)**:
  five unpaced 64 KiB duplex rounds pass byte-exact in 1.759, 1.761, 1.758, 1.762 and 1.758 s,
  delivering 37.2-39.5 kB/s per direction. Neither board adds late preparation/trigger attempts,
  missed boundaries, recovery probes or serial drops. Trigger jitter is at most 3 us. Slots
  become 2990 us, with 5980 us alternating pairs. The same reservation before SRAM relocation
  failed or slowed severely; that result remains diagnostic history, not the current default.

- **511-byte FLRC frames (implemented; both LF boards checked 2026-10-02)**:
  protocol version 3 expands the radio frame capacity to 511 bytes, giving 502 user bytes
  without an ACK extension or 500 with one in P1. LoRa remains capped at 255 bytes. The
  public RAL already allocates separate extended FIFOs; no upstream changes or FIFO streaming
  are needed. Frame encoding checks its wide length sum before narrowing it. All 2198 host
  checks pass with ASan/UBSan; LF/HF firmware builds pass. At a 1 ms reservation, five 64 KiB
  duplex rounds pass but add 7/67 late preparations and 13/41 late triggers, so that timing
  is rejected for large frames. At 1.5 ms, all five rounds pass byte-exact in 1.653, 1.612,
  1.660, 1.659 and 1.610 s (39.5-40.8 kB/s per direction), with zero added late attempts,
  missed boundaries, recovery probes or serial drops. Retransmissions add 58/21; these remain
  a throughput cost. Slots are 4540 us, alternating pairs 9080 us. Both peers must upgrade
  together. Overlapping preparation with RX and reducing turnaround still need acceptance.

- **DATA preparation during RX (implemented; both LF boards checked 2026-10-02)**:
  immutable next-slot DATA is encoded and authenticated without advancing sender or hopping
  state, then written to the separate TX FIFO while RX remains active. The assigned slot
  validates the packet before committing its ARQ window entry. Cumulative ACKs may lag one
  exchange; newer receive progress remains pending. The timeout now allows two alternating
  pairs. Host regressions cover head advancement during RX, stale ACKs, sequence wrap, packet
  immutability during TX, loss, MTU/key changes, timeout and SYNC fallback. LF/HF builds pass.
  Five unpaced 64 KiB duplex rounds pass byte-exact in 1.558, 1.557, 1.613, 1.612 and 1.558 s
  (40.6-43.5 kB/s per direction) at the unchanged 1.5 ms reservation. Neither board adds late
  attempts, missed boundaries, recovery probes, serial drops or bad tags. Trigger jitter is
  at most 3 us. The FIFO is preloaded 674/766 times and reused 664/711 times; maximum preload
  duration is 613/578 us. Retransmissions add 66/25. This validates overlap; tighter reservations
  and lower turnaround costs still need hardware acceptance.

- **Prepared retransmissions and full-window staging (implemented; LF experiment 2026-10-02)**:
  a predicted timeout can prepare the retained head during RX; its assigned TX increments
  retry accounting without adding a window entry. A full window can stage a new tail before
  the current RX ACK releases an entry. All 2263 host checks pass, including two regressions
  for these cases. A 500 us experiment before this change times out with 10.3/19.4 KiB
  received. With the change, all five 64 KiB duplex rounds pass byte-exact in 1.856, 1.763,
  1.812, 1.910 and 1.901 s, with no serial drops, recovery probes or bad tags. However,
  preparation delays add 47/47 and trigger delays 27/30, so 500 us is not accepted as the
  default. The stable 1500 us reservation remains; SPI/DMA and RX completion costs remain open.

- **Radio/DMA helper text in SRAM (implemented; both LF boards checked 2026-10-02)**:
  ZLRS uses Zephyr's relocation build API for the compiled public radio and DMA helpers;
  upstream files are unchanged. The linked DMA ISR/config/start, FIFO reader and RAL payload
  reader reside at SRAM addresses. This adds about 9.9 KiB of RAM. LF/HF builds pass. At the
  same experimental 500 us reservation, five 64 KiB duplex rounds pass byte-exact in 1.658,
  1.657, 1.662, 1.657 and 1.656 s (39.4-39.6 kB/s per direction). Late preparations still
  add 14/19 and triggers 2/0; the reservation remains unaccepted. No serial drops, recovery
  probes or bad tags occur. Further completion/turnaround work is required.

- **DATA occupancy diagnostics (implemented; both LF boards checked 2026-10-02)**:
  successful TX completion counts DATA packets, their application bytes including retransmissions,
  frames at the modem's maximum length, and ACK-only DATA replies. Five 64 KiB duplex rounds at
  1 ms preparation pass byte-exact in 1.660, 1.553, 1.555, 1.605 and 1.554 s. The two senders
  carry 363170/330814 user bytes in 914/938 DATA packets: about 397/353 bytes per packet,
  compared with the 500/502-byte P1 limit. Maximum-length frames account for 266/58 packets;
  retransmissions add 93/10. This establishes incomplete packet occupancy, but does not isolate
  its source. Deferring short RX-prefetched packets and early TX until the preparation window
  does not improve it: five rounds pass in 2.219, 1.707, 1.661, 1.660 and 1.662 s, with mean
  payloads about 337/308 bytes. That policy is reverted. All 2267 host checks pass with ASan/UBSan;
  LF/HF production builds pass. USB input availability, queue aggregation and ARQ stalls still
  need separate measurements before choosing a policy.

- **1 ms preparation with 511-byte preloading (diagnostic; LF checks 2026-10-02)**:
  after restoring the original short-packet policy and updating both peers, five 64 KiB duplex
  rounds pass byte-exact in 4.365, 1.613, 1.560, 1.562 and 1.807 s. Neither board adds late
  preparations, late triggers, missed boundaries, serial drops or fragment loss. However, the
  cold first round pauses for roughly 2.8 s before delivering most data, and each peer adds
  one ARQ recovery. Subsequent directions deliver 36.3-42.0 kB/s. This does not establish a
  stable tighter default; preparation remains 1500 us. The cold-start recovery and its relation
  to phase alignment, packet preparation and ACK timing need isolated measurements.

- **Final 1.5 ms build and resumed-link check (LF hardware 2026-10-02)**:
  both devices run the verified, confirmed image
  `8ce2d5c85b8d78ffbf3013145cd02d0bc0c745201d766dee19a2458bff8ed652`.
  Immediately after sequential updates, the first 64 KiB duplex attempt fails at the host's
  3 s write timeout with no data delivered. RX records authentication failures while TX
  exhausts its ARQ retries. Thus cold-start/session recovery also needs acceptance at 1.5 ms;
  the shorter reservation is not established as its cause. Entering and exiting management
  on both peers restores delivery without a physical recovery. After draining residual bytes
  from the failed attempt, five fresh 64 KiB duplex rounds pass byte-exact in 1.557, 1.555,
  1.554, 1.510 and 1.555 s (42.1-43.6 kB/s per direction). Neither peer adds preparation or
  trigger delays, missed boundaries, serial drops, authentication failures, fragment loss or
  ARQ recovery. Trigger jitter is at most 2 us. Retransmissions add 91/16. Mean application
  payloads are about 461/409 bytes. This accepts the short resumed-link throughput check,
  not unattended cold-start recovery, long runs, hopping or the theoretical throughput goal.
  The bench uses fixed 2441 MHz, hopping off, P1 and -10 dBm. LF/HF production builds, all
  2267 ASan/UBSan host checks and architecture gates pass.

## To-do

### DATA session confirmation, 2026-10-03

- DATA confirmation now remains pending until the peer returns a newer frame authenticated
  with the session key. RX answers with an empty DATA ACK, including when ARQ or reverse data
  is absent. Repeated newer confirms request another reply without resetting stream progress;
  both peers use the sync channel for 16 frames from the last shared confirm index. Pending
  confirmation also retains acquisition-period SYNC, so rebooting RX during confirmation does
  not strand it behind an obsolete candidate key. The initial sticky-confirm experiment omitted
  that SYNC and failed on sequential hardware updates; it is not the retained implementation.
- All 2367 ASan/UBSan host checks pass. The new loss/failure regression fails 37 checks against
  the former implementation. Cases include failed confirm TX, lost confirms and empty replies
  beyond 500 ms, ARQ on/off, hopping on/off, replay/corruption, preserved receive progress and
  RX reboot during pending confirmation. LF/HF production builds and architecture gates pass.
- Both LF boards run verified, confirmed image
  `d0a1227718ba0ae31c634ee3f250764f6d4725eaf0e38d118d014983687de78b` at fixed 2441 MHz,
  P1, -10 dBm, hopping off and 1.5 ms preparation. After sequential updates, all five 64 KiB
  duplex rounds pass in 1.561, 1.504, 1.558, 1.555 and 1.556 s, with no added preparation/trigger
  delays, missed boundaries, ARQ recovery, serial loss or bad tags; trigger jitter is at most 3 us.
  After issuing software cold reboot commands for RX, TX and both peers, three 64 KiB duplex
  rounds each pass without management entry: respectively 1.554/1.557/1.513,
  1.660/1.508/1.556 and 1.656/1.509/1.559 s. Those scripts did not verify reset execution and
  TX shell snapshots are missing, so they do not establish TX reboot or complete two-peer timing
  deltas; RX adds one late preparation in the both-peer reboot run. Physical
  power cycling, repeated long runs and hopping hardware acceptance remain open.

### Radio and protocol

- **Idle DATA hopping (implemented; host checked)**: the DATA receiver keeps its hop epoch
  through legal silence and returns to the SYNC channel after two missed periodic SYNCs.
  The 100 ms SEARCHING repeat timer no longer makes a connected receiver abandon hops before
  its next scheduled SYNC. A simulation with 600 ms periodic SYNC and 180 ms idle gaps shows
  sixteen failures against the old implementation; eight prompt transfers pass with the fix.
  Replay/corrupted frames do not refresh the trusted epoch, and 205 receive-window checks
  cover reacquisition across frame-counter wrap. LF/HF builds pass; hopping hardware acceptance
  remains pending.

- **Consistent planned-slot time (implemented; LF checked 2026-10-03)**: TIMER1 deadlines map
  to protocol milliseconds through one uptime epoch and the timer's full 64-bit counter.
  Preparation and activation of a slot now use the same millisecond, including timer/kernel
  wrap. Previously two rounded `uptime_ms + remaining_us / 1000` calculations could differ
  by 1 ms and change a preloaded retransmit decision. Seven host cases cover this rounding,
  past deadlines and both wraps; LF/HF builds pass. At experimental 1 ms preparation, five
  256 KiB board-source/sink rounds and five 64 KiB USB rounds pass every byte. RF goodput is
  50.0-60.7 kB/s per direction, but the RF run still adds seven late preparations and one
  late trigger. This fixes the time mapping, not tight-slot acceptance; the shipped default
  remains 1.5 ms.

- **Reuse the FLRC synthesizer (LF checked 2026-10-03)**: after a successful single RX/TX,
  the configured hardware fallback leaves the radio in FS. An identical FLRC request keeps
  that synthesizer locked while clearing IRQs and accessing the FIFO. Frequency, modem, packet
  length/format, sync word or power changes, chip errors and aborts use the standby setup path.
  Five 64 KiB board-source/sink duplex rounds and one 256 KiB round pass with every pattern byte
  checked, P1 and ARQ enabled. Receive goodput remains 44.5-54.7 kB/s per direction at the
  unchanged 9080 us pair; five 64 KiB USB rounds also pass in 1.958/2.008/1.956/1.958/1.958 s.
  Full preloaded TX snapshots show preparation stages of about 30/5/0-1/4 us
  (IRQ/configuration/FIFO/BUSY) and thousands of FS reuses, without new timing failures in these
  runs. Image `2c36d9b334ee7fb62a40f0d98a7f0a194b4e9c08fe65cb28b6cf9b808e398c33`
  is confirmed on both LF peers. All 4487 host checks and LF/HF production builds pass.
  This removes repeated standby/PLL work; tighter-slot and hopping acceptance remain open.

- **Debug console and verified software reboots (LF checked 2026-10-03)**: flash-only logging,
  polling console TX and an 8 KiB CDC FIFO pass twelve open/query/close cycles on each board.
  Image `0be39554be787a725dfc9bd02a5551f823f8b55a355ecd32d197f0e8cf12cb1e` is confirmed
  on both peers. Five 64 KiB board-source/sink duplex rounds pass at 46.5-54.3 kB/s per
  direction; five USB rounds pass byte-exact in 1.910/1.909/1.855/1.860/1.857 s. These do
  not establish a USB speed gain. Uptime resets verify actual RX, TX and both-peer software
  reboots; their three 64 KiB USB rounds pass in 1.905/1.853/1.862, 1.906/1.861/1.862 and
  1.861/2.012/1.862 s, with complete statistics from both peers. During these transfer checks,
  neither peer adds late preparation/trigger, missed boundaries or slots, ARQ recovery or
  stream loss; launch jitter stays within 3 us. Acquisition itself can add a late preparation
  or missed slot before measurements. USB/IP preserves device paths across these resets, so
  disappearance is not a reliable reset check. Physical power cycles and extended runs remain open.

- **Bulk stream queue copies (implemented; LF hardware checked 2026-10-03)**:
  queue insertion and packet extraction use at most two contiguous copies across a ring wrap,
  updating occupancy once. All 4487 ASan/UBSan checks pass, including eight cases covering ring
  starts, split insertion, ARQ/best-effort delivery and prepared packets; saturation accounting
  remains covered. LF/HF builds pass. The debug source/sink supplies full queues while checking
  every received byte. Five 64 KiB duplex rounds pass, with receive goodput 47.8-54.7 kB/s per
  direction, compared with 45.6-54.3 kB/s before bulk copies. Five USB duplex rounds also pass
  byte-exact in 1.503, 1.554, 1.510, 1.605 and 1.552 s. This does not establish timing acceptance
  for tighter slots. The current 9080 us pair permits about 55.1 kB/s per direction with a
  500-byte payload before losses: reserved time still limits the radio even with full queues.

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
