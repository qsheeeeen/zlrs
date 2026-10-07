# Roadmap

This document holds **to-dos, the pending-defaults ledger and acceptance criteria** only.
Finished work gets a one-line summary; details live in code comments and the design is in the
design documents.

## Protocol crypto and hardware SHA separation, 2026-10-06

- Removed the PSA protocol backend, protocol Kconfig dependencies and application PSA
  initialization. `core/proto/crypto.c` now owns HMAC, HKDF, PBKDF2 and protocol derivation rules.
  `app/src/platform/sha256.c` supplies one SHA operation through the existing Zephyr driver.
  Firmware requires the SHA peripheral/driver; there is no software fallback or runtime selector.
- Host tests link the same protocol algorithms with `tests/host/sha256_soft.c`. The existing
  hardware crypto cases now mirror `core/proto` and retain the firmware's SRAM placement.
  Wire tags, key derivation parameters and algorithms retain their existing rules.
- HF production and LF debug application/MCUboot builds pass in the normal `build` directory;
  the build tree is left on LF debug. Application PSA and Mbed TLS are disabled, no PSA sources
  are compiled, and protocol crypto, SHA access and the driver remain in SRAM. Test execution,
  hardware tag vectors, pairing, timing and bidirectional delivery after this separation remain
  pending.
- Security review follow-ups: prevent old authenticated frames from passing the signed 16-bit
  sequence comparison after a session advances by half the sequence space; define the acceptable
  forgery budget for 32-bit tags; propagate invalid derivation arguments as errors and make
  sensitive-data clearing consistent. Removing PSA also removes its wrapper's zero-tag-on-error
  path. These protocol/interface issues are not fixed by separating the source files.

## Radio integration cleanup, 2026-10-05

- Removed the CMake code that inspected upstream targets and excluded their HAL source.
  Ordinary radio commands and FIFO I/O now use USP's original HAL and Zephyr SPI driver.
  `radio_io.c`, its Kconfig option and custom DMA diagnostics are removed.
- SRAM placement lists only ZLRS-owned application, protocol and SHA driver sources.
  No build rule scans upstream library source lists for replacement or relocation.
- TIMER1 slot scheduling and its prepared SET_TX trigger are separate from this HAL change.
  The trigger still uses the existing bounded register polling in the launch ISR.
- Earlier custom-HAL/DMA throughput records below describe the previous integration.
  LF/HF application and MCUboot builds pass after restoration. Hardware delivery,
  timing and throughput after restoration remain unverified.
- Removing USP entirely would require a ZLRS LR2021 command driver, modem configuration,
  FIFO/IRQ handling, board initialization and calibration. This is a separate design decision.

## Test migration by module, 2026-10-05

- Existing tests now match production modules: portable cases in `tests/host`, mirrored hardware
  paths under `tests/hardware`, and one `test_zlrs*.py` per tool under `tests/tools`. Hardware
  applications only assemble these files; the mapping and commands are in [11-debug](11-debug.md#7-tests-organized-by-module).
- Normal app/core sources no longer contain the entropy smoke test, raw radio loop, generated
  RF stream, rekey/window experiments or test-only mount/raw APIs. Production driver capability
  checks, TRNG health/error handling, status counters and LED patterns remain runtime work.
- The unit image assembles nine cases; radio tests link the actual firmware and replace public
  data/radio interfaces only in the test image. Both test configurations compile for LF, including
  the raw-radio variant; LF/HF normal firmware and the host test binary also compile.
  Repeating hardware acceptance after migration remains pending; earlier
  crypto and duplex records below describe the pre-migration images.
- Checks run directly from `tests/`; the `tools/gates.sh`, tool `--self-test` and CLI `throughput`
  compatibility entries are removed. CI and current usage documents call the test scripts.
  The firmware build rules live in `app/CMakeLists.txt`, which the radio test includes directly.

## Zephyr SHA-256 driver migration, 2026-10-05

- ZLRS owns `drivers/crypto/rp2350_sha256.c`, based on the upstream RP2350 Crypto driver,
  with multipart hashing, context ownership checks and digest restart within a session.
  The board's `zlrs,rp2350-sha256` binding selects this driver without upstream changes.
  At that stage, hardware availability followed `ZLRS_HAS_SHA256` in `app/src/platform/caps.h`;
  there was no application backend option. HMAC/HKDF/PBKDF2 derivation rules stay the same.
- HMAC feeds the key prefix and message through update/compute without joining input buffers;
  the previous 575-byte scratch buffer is removed. Hardware input errors return `-EIO`, and
  an invalid context cannot release another session's ROM lock.
- HF production application and MCUboot compile in the default `build` directory; the SHA
  driver and protocol crypto text are in SRAM. Both LF boards initially passed 38/38 known-answer
  vectors in five runs per board. With RF paused, aligned 507-byte tags take 102-105 us versus
  86 us in the previous direct SDK baseline; the migration adds driver/session overhead.
  Hardware tests now live in `tests/hardware/unit`, a separate Ztest application, and are
  absent from application/driver sources. Before consolidation, both LF boards passed the six
  crypto Ztest cases, including
  session/error checks and 100000-round passphrase derivation (4307/4308 ms); the runner verifies
  rollback to the previous confirmed image. The standalone test uses minimal libc to avoid a
  USB DPRAM halfword-store fault in SDK picolibc's memcpy; its 73 us aligned full-frame tag time
  is not an application speedup.
- After removing the embedded tests, both LF boards run the confirmed application image
  `da762608943792c8a3e58342e3b96d25150d58f549f56111ed5ba5a78f6529b8`.
  At 2441 MHz, -10 dBm and P1, three USB duplex rounds deliver 65536 bytes per direction
  exactly (42.1-48.3 kB/s); three RF-only duplex rounds deliver 262145 bytes per direction
  with zero bad bytes (45.2-46.5 kB/s). Neither run adds timer misses, skipped frames or late
  launches or authentication failures; SPI errors, TX FIFO underflows and chip errors stay zero.
  CRC failures and ARQ retransmissions remain, so these runs establish functional delivery,
  not a throughput improvement. HF hardware and real-UART acceptance remain pending.
- The subsequent crypto separation removes PSA from the protocol path; hardware PSA integration
  is no longer part of this prototype's implementation plan.

## Product direction consolidation, 2026-10-04

The [product page](01-product.md) describes what ZLRS is and how it is used. This roadmap keeps
the implementation gaps, unresolved performance decisions and hardware evidence. The product has
three operating shapes: CRSF control and telemetry only, bidirectional data only, and both together.
CRSF control and telemetry are one service; the roadmap records the work needed to make each shape
complete and measurable. Further prototype edits and hardware experiments remain tied to the product
direction recorded here.

- A dedicated 1 ms control experiment is not accepted as 1 kHz control. Its latest 10 s USB/IP
  run supplied 8470 samples and delivered 6391 distinct samples (639.1 fresh updates/s), with
  telemetry and hopping disabled. Timer cadence, repeated control frames and fresh delivery
  are separate measurements; this experiment does not resolve the host/radio loss split.
- The new mixed-mode target allocates alternating 1 ms control and 1 ms data windows.
  Whether 500 Hz DATA is the total or each-direction rate remains to be clarified.
- After the remaining performance decisions are resolved, prepare complete per-mode timing budgets
  and compare frame, ARQ, security and scheduling designs before continuing implementation.

### 待收敛的产品决策

产品页 [01-product](01-product.md) 已确定通道方向：固定 CH1–CH4 为主通道，
每次有效控制更新均更新；CH5–CH16 为辅助通道，高速档位下每通道最低新更新率 50 Hz；
全部 16 个通道保留 11 bit 精度。低速档更新率、最大更新间隔与样本年龄仍待定；本轮
暂缓急停、解锁的特殊发送优先级与响应时限。以上方向已经确定，硬件状态见下文。

产品页也确定遥测职责：设备生成自己的链路统计，外部飞控遥测通过
CRSF 端口输入后反向转发，保留消息类型与业务载荷，无需解释飞控业务含义。两类遥测
均属于 CRSF 业务。自产链路统计的刷新目标为 10 Hz，名义周期 100 ms，
适用于单 CRSF 与并发模式；统计输出延迟、最大更新间隔与外部遥测的容量、延迟、
允许丢失及拥塞规则仍待收敛。

无线档位与在线切换方向也已确定：分别定义近距高速与远距档，高速档主控制
新更新目标为单 CRSF 1000 Hz、并发 500 Hz。首版固定最快档；后续提供跨重启保存的
自动切换使能配置，默认关闭，关闭时固定最快档，开启时自动选档，不提供手动选档或
切档。远距档服务量、使能配置的生效规则、两档无线条件与切换连续性阈值继续收敛。

| 编号 | 待定项 | 为什么会影响方案 |
| --- | --- | --- |
| Q01 | 控制+遥测+数据的数据 500 Hz 是双向总计，还是每个方向各 500 Hz | 决定每窗口发送次数、反向机会、MTU 与换向预算 |
| Q02 | 单 CRSF 1000 Hz、并发 500 Hz 的新主控制样本交付目标所需测试条件、允许丢失与维护占用 | 新样本交付口径已明确；需要同时满足辅助通道与遥测指标，确定最大控制间隔 |
| Q03 | 低速档辅助通道更新率，以及各档最大更新间隔与样本年龄；高速档已定每辅助通道 50 Hz | 决定辅助通道调度、丢包余量与时序验收；主通道集合与 11 bit 精度已确定 |
| Q04 | 自产链路统计的输出延迟与最大更新间隔，以及外部遥测转发的容量、延迟、允许丢失与拥塞规则 | 链路统计目标 10 Hz 与两类遥测职责已确定；资源预算与该档位控制指标一起验收 |
| Q05 | 三种模式分别要求的最小吞吐、最大延迟、抖动和丢失上限 | 没有这些值就无法判定候选方案是否合格 |
| Q06 | 首版各模式固定高速档的频段、无线参数、服务量与距离条件；后续远距档及自动切换的组合 | 首版用于作用距离与性能验收；后续用于配置矩阵、换频成本与切换预算 |
| Q07 | 加密是否是当前版本必需，认证目标是什么 | 决定协议开销与硬件计算预算 |
| Q08 | 失联保护、缓存、重启恢复与长期运行的数值边界 | 决定状态机、队列、ARQ 和接受旧数据的规则 |
| Q09 | 必须在配置工具中开放的模式、频段、频率、速率与遥测参数 | 决定协商、配置版本、状态反馈与固件裁剪的范围 |
| Q10 | 本阶段必验硬件/端点，以及距离、功率、干扰和测试时长 | 决定哪些证据足以验收；LF/USB 实测不能替代 HF/真实 UART |
| Q11 | 后续自动切换的使能配置生效规则、双端配置关系、触发条件、控制空窗、数据处理与失败恢复阈值 | 首版固定最快档与后续配置使能已确定；自动切换仍需两端协调、业务连续性和稳定性验收 |

Q01、Q02 的频率口径与 Q04 的遥测规格继续收敛；其余项可以逐项补充，但在对应方案
开始实现前必须有明确目标或显式标注的阶段范围。旧文档里的距离目标、帧格式和默认值
只能作为参考，不能填补用户尚未确定的需求。

Earlier distance targets (hundreds of metres for racing and approximately 2 km or more
for sub-G operation) also require per-mode RF, antenna, power and acceptance conditions.

### 当前实现差距

| 项目 | 当前情况 | 与规格的关系 |
| --- | --- | --- |
| 管理运行模式 | 已提交实现支持 mixed / data24；单控制+遥测主要是构建裁剪 | 不等于三种模式都有完整运行时配置 |
| 独立 2.4 GHz 数据 | 已有 LF 双板双向完整交付记录 | 证明部分功能；最终吞吐、长期稳定及其他硬件仍需验收 |
| 1 ms 控制实验 | 工作区有未提交的控制原型，两板当前运行该实验镜像；遥测/跳频关闭 | 是隔离实验；完整 CRSF 模式须启用遥测验收，不得标成 1 kHz 功能已完成 |
| 控制+遥测+数据的新时序 | 只有需求草图，当前 mixed 实现仍是旧计划 | 500 Hz + 500 Hz 尚未实现或通过验收 |
| 外部 CRSF 遥测转发 | 当前应用只消费 0x16 控制输入并生成 0x14 链路统计，其他输入帧未进入无线遥测转发 | 产品页已定义外部遥测透明转发；功能尚未实现，需覆盖 USB／UART 输入、业务载荷保留与并发服务量 |
| 无线档位在线切换 | 现有 FLRC rate adaptation 默认关闭，阈值为占位值；自动切换使能配置与完整切换验收仍缺失 | 产品页已定义首版固定最快档、后续配置使能自动切换；仍需验证双端一致性、切换控制空窗与数据交付 |
| 安全 | 当前 P1 认证与防重放；P2 加密未实现 | 不应把认证描述为数据保密 |

### 后续工作顺序


1. 按单控制+遥测、单数据、控制+遥测+数据逐项收敛需求表，补全适用条件与验收阈值。
2. 编制预算表：逐模式、逐方向列载荷、空口、SPI、认证、启动、切换和排队成本；
   明确哪些能重叠执行、哪些必须串行，避免重复计时或遗漏关键路径。
3. 比较整体方案：时序、帧字段逐 bit 布局、ARQ、安全、FIFO 与调度一并设计，列出
   每个方案满足哪些需求、还差什么以及预计余量。
4. 选择方案后再实施：每批修改对应具体规格和验证目的；硬件结果更新路线图。

没有收敛的规格，不以调整一个常量、缩短一个字段或临时跑通一次替代设计完成。

## First performance milestone: 1 kHz CRSF control, 2026-10-04

Prioritize 1 kHz CRSF control before maximizing bidirectional DATA24 throughput. Shorter
radio frames and a higher proportion of framing overhead are acceptable to achieve stable
control timing. Protocol fields, channel encoding, PHY and driver/scheduler choices remain
open under the system-wide design brief below.

- Define a 1000 us control cadence and measure actual fresh RC delivery, launch jitter,
  sample age and CRSF output timing separately. Repeating an old sample does not establish
  a 1 kHz input-to-output update rate.
- Evaluate asymmetric uplink/telemetry scheduling. Local ExpressLRS F1000 configuration
  uses a 1000 us RF interval, an 8-byte OTA packet and a suggested telemetry ratio of 1:128.
  Its TX timer skips uplink transmission in telemetry slots: this permits at most
  992.1875 uplink slots/s and 7.8125 telemetry slots/s before SYNC or other uplink traffic.
  F1000 carries four 10-bit main channels in every RC packet and multiplexes auxiliary
  channels; it does not send all sixteen 11-bit CRSF channels every millisecond.
  See [telemetry allocation](https://www.expresslrs.org/quick-start/transmitters/lua-howto/#packet-rate-and-telemetry-ratio)
  and [channel encoding](https://www.expresslrs.org/software/switch-config/).
- If telemetry replaces a control slot, report the resulting control rate and longer update
  gaps explicitly. To retain a control transmission every millisecond, fit reverse traffic
  and both direction changes between control deadlines or evaluate a faster underlying cadence.
- As an initial budget candidate, the existing 2.4 GHz FLRC parameters (2.6 Mbps, CR 3/4,
  24-bit AGC preamble, 16-bit sync, variable length and CRC16) give 162 us calculated airtime
  for 22 bytes of CRSF channel data plus the current 7-byte P1 framing. This excludes radio
  startup, SPI, validation and turnaround. A 26-byte CRSF UART frame at 420000 baud with
  8N1 occupies about 619 us per port; include sustained input/output capacity and pipeline
  latency in acceptance. These calculations are not hardware acceptance or a required format.
- Hardware acceptance remains pending: sustained fresh control updates on both boards,
  selected telemetry policy, timing tails, loss/failsafe behavior and unattended recovery.

### Mixed-mode timing target, 2026-10-04

The user sets a separate mixed-mode target: alternate a 1000 us control window and a
1000 us data window. A complete cycle is 2000 us, giving 500 control windows/s and
500 data windows/s. Whether DATA must transmit 500 times/s total or in each direction
is unresolved in the pending specifications above. Dedicated control retains the
1 kHz target; data-only operation keeps an independently sized throughput plan.

- With one data packet per window, alternating data directions gives 250 opportunities/s
  per direction; fitting both directions inside one window requires a separately measured
  turnaround budget. Do not assume which allocation the user requires.
- Control uses the newest sample without retransmitting old control samples. Data
  remains reliable; aggregate ACKs and link control in shared frame headers, including
  header-only reverse packets when there is no user data. Telemetry must fit the chosen
  reverse allocation without silently replacing control opportunities.
- Derive a smaller MTU from the complete 1000 us window, including preparation, SPI,
  radio startup, airtime, receive validation and deadline margins. The current 511-byte
  frame cannot fit. Decide band assignment and account for band-switch costs explicitly.
- Measure control delivery timing as well as launch timing. Variable-length data appended
  to control delays whole-frame CRC/authentication completion; aggregation must not create
  unbounded control latency or jitter.
- This is a design target, not implemented or accepted hardware behavior. The existing
  mixed scheduler still uses its previous plan until the new schedule is validated.

## DATA24 performance design brief, 2026-10-04

The user requests a system-wide redesign toward the maximum measured bidirectional DATA24
throughput. Existing implementation choices are candidates to replace. Every protocol field,
its width and even its presence are open to reconsideration, including transmission counters,
data sequences, ACKs, authentication tags, CRCs, framing, packet types and handshake messages.
The same applies to ARQ/window policy, MTU, FIFO use, buffering/copies, DMA, thread ownership,
interrupts, scheduling, slot count, turnaround and transport integration. Earlier 8-byte or
7-byte frame sketches are candidates, not requirements. Compatibility with existing modes or
formats does not constrain this exploration. Keep the resulting design clear and maintainable;
make integration changes inside ZLRS without changing or patching other repositories, and keep
hardware recovery unattended.

Research directions and acceptance:

- **Alternating confirmation bit**: evaluate an ELRS-inspired ACK that changes on acceptance of
  each new data block. A stop-and-wait design can use one transmit phase bit and one reverse ACK
  bit in the header, repeating the phase on retransmission and withholding duplicate delivery.
  ELRS's confirmation is one bit; its actual DATA header also carries a package index. Neither
  that index nor our four-frame window is a required design choice. Define initialization,
  lost data/ACK handling, delayed duplicates, idle periods, resynchronization and peer reboot
  behavior as part of the candidate protocol. Compare alternatives on measured goodput.
- **Compact unified framing**: use bits for flags and bounded values, infer fields from agreed
  context when possible, aggregate packet types, and place link control in the header. Carry
  reverse ACK/control in a header-only packet when no user data is available. Reconsider each
  field's purpose before choosing its encoding; do not merely shorten the current format in isolation.
- **Whole-frame FIFO transfers**: compare one complete TX FIFO write and one complete RX FIFO
  read per packet with streaming transfers. The current vendor initialization enables separate
  extended TX/RX FIFOs large enough for a 511-byte frame. Capacity is not the reason for the
  present 128-byte TX prefix. Measure transaction count, copies, CPU work and critical-path time.
- **Joint protocol and scheduling design**: consider packet preparation, authentication, FIFO
  loading, receive validation, ACK generation and the next launch together. A one-bit ACK
  candidate needs timely reverse confirmation; the current preload can carry an ACK one exchange
  behind, so adopting it unchanged could waste a complete exchange. Neither a particular preload
  policy nor a fixed preparation/guard reservation is a prerequisite.
- **End-to-end evidence**: derive throughput bounds from hardware capabilities and the chosen
  PHY/protocol, then account for air occupancy, turnaround, ACK latency, aggregation, queue
  stalls, USB/UART transport, copies, DMA, interrupts and thread scheduling. Use board-generated
  traffic and host-to-host traffic to separate costs. Compare whole designs under matched loads,
  with byte-exact delivery, timing, loss, backpressure and unattended recovery measurements on
  both boards. Reaching the current slot plan's ceiling does not establish the physical limit.

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
  attempts. IRQ-to-link-completion and protocol-call timings are available in the management
  `radio` report;
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

- **Word-aligned frame authentication (implemented; both LF boards checked 2026-10-03)**:
  TX/RX buffers and HMAC blocks use the SDK's word input path. Both boards pass all 38 hardware
  vectors repeatedly without resetting; 507-byte tags average 84-87 us aligned versus about
  123 us unaligned. Large diagnostic buffers stay outside the shell stack, leaving 692 bytes
  free on each board. All 23908 host checks and LF/HF builds pass. The 500/750 us preparation
  experiments still reject occasional requests, so the diagnostic image retains 1 ms.

- **Fill fallback FLRC TX FIFO during transmission (implemented; LF checked 2026-10-03)**:
  fresh TX preparation writes at most 128 bytes, and the RF owner appends the authenticated
  tail after the hardware timer issues SET_TX. Matching RX-time preloads remain complete.
  Public RAL FIFO writes and underflow IRQs are used; no upstream changes are needed. Tail
  I/O failure or underflow fails the transaction, and abort/completion resets prefix state.
  All 23905 ASan/UBSan host checks pass, including 2560 new prefix/tail checks; LF/HF builds
  and gates pass. At fixed 2441 MHz, P1, -10 dBm and 1 ms preparation, six 262145-byte RF
  duplex rounds pass every byte at 55.7-61.1 kB/s per direction. Five 64 KiB USB rounds pass
  in 1.703/1.560/1.559/1.556/1.557 s. Steady RF and USB runs add no late preparations,
  late triggers or missed boundaries/slots; TX jitter is at most 3 us. Both boards append
  tails, with no I/O errors/underflows and completion within 507 us of launch. Uptime-proven
  simultaneous cold reboots are followed by three exact 64 KiB duplex rounds in
  1.759/1.604/1.557 s with no added timing failures. Initial modem setup still rejects one
  late preparation on TX at boot; these checks do not accept a smaller preparation window
  or a USB throughput gain.

- **SPI DMA timing (implemented; LF checked 2026-10-03)**: native HAL diagnostics report the
  actual 15 MHz SCK and separate DMA setup, active transfer, owner wake, finish and preceding
  BUSY wait. Three 262145-byte RF duplex rounds and three 64 KiB USB rounds pass. During the RF
  run the peers add 8205/8620 DMA transfers of about 209-210 bytes on average, with no DMA errors;
  mean setup/active/wake/finish/BUSY costs are about 6.5/142/21/0.35/10 us. Active includes driver
  start and IRQ dispatch. Reconfiguring DMA is a small part of the measured cost; full FIFO
  writes and packet construction remain the preparation path to shorten. LF/HF builds pass.

- **Read the RX FIFO during FLRC reception (implemented; LF checked 2026-10-03)**:
  a 128-byte FIFO threshold wakes the RF owner, which copies decoded prefixes through public
  RAL APIs while the remainder is on air. CRC/tag checks still precede delivery. Restart,
  abort and completion discard the prefix count; final-length bounds prevent overflow or
  rereading the prefix. A coalesced FIFO/RX_DONE IRQ cannot adjust phase using its early
  timestamp. The 8217 new host bounds checks pass; all 21345 ASan/UBSan checks pass.
  Three 1048577-byte board-source/sink duplex rounds pass at 55.8-60.7 kB/s per direction,
  without new late preparations, late triggers or missed boundaries/slots. RX completion
  snapshots fall from roughly 1.1-1.6 ms to 0.2-0.34 ms. CRC errors and one FIFO payload
  rejection occur during this run; ARQ still delivers every byte with no pattern errors.
  Five 64 KiB USB rounds pass in 1.555-1.661 s, so this is reduced completion latency,
  not an accepted USB throughput gain. Verified both-peer software reboots then pass three
  64 KiB USB rounds in 1.609/1.562/1.610 s, with no new late preparations/triggers or missed
  boundaries/slots and trigger jitter within 3 us. USB/IP auto-attach is needed for both boards.
  LF/HF production builds pass; upstream is unchanged.
  The accepted experiment retains 1 ms preparation and fixed 2441 MHz.
  A 0.5 ms preparation experiment delivers five 262145-byte RF rounds at 60.3-68.7 kB/s,
  but adds 39/19 rejected preparations; USB adds another 44/29 and takes 1.604-1.709 s.
  Fresh packet construction plus SPI still takes about 0.7-0.8 ms when a staged packet
  cannot be used. The shipped 1.5 ms reservation remains.

- **Management RX wakeup (implemented; LF checked 2026-10-03)**: received bytes signal a
  semaphore instead of waiting for the next 50 ms poll. The idle housekeeping bound and
  synchronous `ENTER` log flush remain. RX overflow counts bytes rather than chunks.
  Both peers export all seven retained log files (798252/815744 bytes); the latter export
  completes in about 41 s. After export, five 64 KiB USB duplex rounds pass in
  1.557-1.561 s, and three 262145-byte board-source/sink rounds pass at 55.3-61.1 kB/s
  per direction. LF/HF production builds pass. These measurements use the experimental
  1 ms preparation and fixed 2441 MHz, with CDC traffic INFO logs disabled.

- **Quiet CDC traffic logs (implemented; LF checked 2026-10-03)**: the public
  `USBD_CDC_ACM_LOG_LEVEL_WRN` setting removes per-payload INFO dumps and FIFO wakeup messages.
  The former level floods the deferred queue and rotates files during their own export;
  hardware exports fail at offsets 8640 and 37920 with the oldest listed file already gone.
  Five 64 KiB USB duplex rounds improve from 2.104-2.159 s to 1.454-1.560 s at unchanged
  1 ms preparation, with every byte checked. Three 262145-byte RF rounds also pass; this
  is a serial-path gain rather than a higher PHY rate. Together with event-driven management
  RX, complete 798252/815744-byte log stores export successfully from the two peers.
  LF/HF production builds pass, and no upstream source changes are needed.

- **Arm TX only in advance (implemented; LF checked 2026-10-03)**: preparation and TIMER1
  arming require a 25 us lead. An expired alarm is rejected instead of forcing the interrupt;
  ISR entry more than 10 us late is rejected before SPI. Fourteen host checks cover the lead
  boundary, invalid margins, past targets and counter wrap; all 13128 ASan/UBSan checks pass.
  Five 1048577-byte board-source/sink duplex rounds pass at 56.2-61.2 kB/s per direction.
  Trigger jitter stays within 2/3 us; neither peer adds late triggers or missed boundaries/slots.
  RX rejects five preparations that miss the margin, so this does not establish zero missed
  preparation deadlines. Five 64 KiB USB rounds also pass in 2.105/2.106/2.159/2.108/2.104 s.
  Verified both-peer, RX and TX software reboots each pass three 64 KiB USB rounds, with
  trigger jitter within 2 us and no new late triggers; the RX reboot transfer adds one
  rejected preparation. The experiment still uses 1 ms preparation and fixed 2441 MHz;
  shipped default and hopping acceptance remain unchanged.

- **One protocol clock (implemented; LF checked 2026-10-03)**: TIMER1 now timestamps actual
  completions, timeout ticks, acquisition and management-triggered pairing as well as planned
  slots. Hardware snapshots show kernel uptime falling another 12 ms behind TIMER1 during a
  30 s RF run. Mixing these clocks shortened ACK ages and triggered unnecessary retries.
  With one clock, five 262145-byte RF duplex rounds pass at 56.2-61.1 kB/s per direction,
  remaining steady across rounds. Reverse retries fall from 675 to 33 and duplicate DATA
  received by TX from 645 to 11; mean confirmation delays are 16.0/17.9 ms. During those
  RF and five 64 KiB USB rounds neither peer adds late preparation/trigger or missed
  boundaries/slots. Verified RX, TX and both-peer software reboots each pass three byte-exact
  64 KiB USB rounds; the RX-only reboot transfer still adds one rejected late trigger, and
  some attempts show up to 52 us jitter. These checks validate the clock repair, not a tighter
  default or end-to-end USB speed gain. Preparation remains 1.5 ms in shipped builds.
  All 13114 ASan/UBSan checks, architecture gates and LF/HF builds pass.

- **ACK pipeline diagnostics (implemented; LF checked 2026-10-03)**: DATA ACK coverage,
  stale staged ACKs, full-window stalls, retry age and first-send confirmation delay are
  exposed by the management `link` report. Host cases cover cumulative release, staging, exact
  retry ages and
  clock wrap; all 13114 ASan/UBSan checks and LF/HF builds pass. Five 262145-byte RF duplex
  rounds pass byte-exact at experimental 1 ms preparation, with 44.5-57.6 kB/s per direction
  and no added late preparation/trigger or missed boundaries/slots. The reverse sender
  retries 675 times and sends 2137 ACK-bearing DATA packets out of 3295. Its measured mean
  confirmation delay is only 0.7 ms because negative elapsed values are clamped to zero;
  planned TIMER1 timestamps and actual kernel timestamps need a consistency check before
  these delay measurements can establish an ACK timeout budget.

- **ARQ clock ordering (implemented; LF checked 2026-10-03)**: timeout checks use signed
  elapsed milliseconds, so an ACK timestamp 1 ms ahead of a planned slot does not become a
  near-2^32-ms timeout. Fifteen host cases cover future ACKs, the exact timeout and clock wrap;
  the old implementation fails nine checks. All 13055 ASan/UBSan checks, architecture gates
  and LF/HF builds pass. Five 262145-byte RF duplex rounds pass every byte at experimental
  1 ms preparation, with goodput 44.5-57.0 kB/s per direction. Reverse retries remain 650
  versus 661 in the preceding run, so this edge-case fix does not resolve the sustained retry
  cost. The RF run adds three late preparations on TX; five subsequent 64 KiB USB rounds pass
  without added preparation/trigger delays or missed boundaries/slots. Tight-slot acceptance
  and ACK/loss attribution remain open.

- **Bulk debug stream patterns (implemented; LF checked 2026-10-03)**: source generation and
  receive checking process four bytes together, retaining the exact positional pattern and
  incorrect-byte count. Host cases cover both roles, unaligned buffers, byte/word boundaries,
  stream-counter wrap and injected corruption. Five 262145-byte board-source/sink duplex rounds
  and five 64 KiB USB rounds pass every byte at experimental 1 ms preparation. Neither peer adds
  late preparation/trigger or missed boundaries/slots during these warm runs. RF goodput ranges
  from 44.1 to 55.9 kB/s per direction; USB rounds take 2.055/2.061/2.058/2.154/2.058 s.
  These measurements establish correctness of the faster pattern implementation, not a
  throughput gain. Reboot and tighter-slot acceptance remain open.

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
  Evaluate an alternating confirmation bit together with its ARQ and scheduling model, as
  described in the DATA24 performance design brief above.
- **Over-the-air firmware update** (optional): images travel over the 2.4G data link, roughly
  10 s per MB.

### Hardware

- **Carrier board spin**: the two-layer board from [04-carrier](04-carrier.md).
- **Single-port variant**: no USB, one UART port whose link role is configured at runtime
  ([02-platform](02-platform.md) 2.1). This needs firmware support that does not exist yet: the
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
  [07-firmware](07-firmware.md) 5): all four paths pass: the first image needs no confirmation,
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
place where "not tested yet" is written down; [11-debug](11-debug.md) owns the switches
themselves.

| Knob | Default today | What is missing | Afterwards |
| --- | --- | --- | --- |
| `ZLRS_APP_RATE_ADAPT` | off | per-tier packet error rate and SNR thresholds, runtime enable configuration and transition acceptance | first release stays at the fastest tier; later opt-in runtime adaptation, default off, with measured thresholds |
| `ZLRS_APP_WATCHDOG` | on | a long run on hardware without false resets (PBKDF2 inside management mode is the slowest feed interval) | stays on |
| `ZLRS_APP_DATA_ARQ` | on | throughput and latency on hardware; the ack can shrink to one header bit once the RX always has a frame to ride along | stays on, window sized from measurements |
| `ZLRS_APP_DYNAMIC_POWER` | on | thresholds and hysteresis from measured RSSI/SNR | stays on, thresholds replaced |
| `ZLRS_APP_SUB_BAND_MIX` | on | switching cost and interference benefit, with two modules | stays on |
| `ZLRS_APP_FHSS`, `ZLRS_APP_SESSION_HANDSHAKE`, `ZLRS_APP_TELEMETRY` | on | the first two-module session (pairing, session, telemetry round trip) | stay on |
| rate tier and frame-rate values, telemetry period | compile time | management-protocol commands to match [07-firmware](07-firmware.md) 3 | runtime, stored in NVS (role and security profile already are) |

The switches that cut the product's *shape* (which links, which band role, which modulation) are
not debt: they stay and any image may set them. The ones that only exist to isolate a layer on
the bench — P0, hopping off, session handshake off, telemetry off, ARQ off, sub-band mixing off,
radio off — are gated by `CONFIG_ZLRS_APP_DEBUG`, so a shipped configuration cannot reach them;
see [11-debug](11-debug.md) 3.

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
