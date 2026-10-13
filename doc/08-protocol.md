# 空中协议与管理状态

本文定义协议 11 的空中格式、数据模型、收发规则及管理状态契约。各端须选择相同版本、频段模式、SubG 区域和 profile；不兼容其他空中版本。密钥与安全边界见[安全设计](09-security.md)，模块职责见[架构](06-architecture.md)。

## 1. 总体格式

```text
TYPE / 该类型首字节字段 | SEQ(u32) | 该类型消息体 | TAG
```

- 八种类型继续用编号 0–7，只需 3 bit。低位 bit0–2 是 type。
- DATA、PAIR、SESS_RESP、SESS_CFM 的首字节高 5 bit 为字节对齐位，必须为零。
- SYNC 的 type 后正好接本类型的 `profile:1` 和 `version:4`，三项共同占首字节：bit3=profile，bit4–7=version。这些值是 SYNC 消息体字段，不是所有帧共有的标志。
- SYNC 的 version/profile 必须与本地选择一致，否则拒绝该握手。
- 本格式只编码 P0=0、P1=1；加密 profile 属于未来协议扩展。版本 11 可用 4 bit 表示；版本超过 15 或增加 profile 时需要明确修订格式，编译期检查必须阻止截断。
- 公共逻辑字段是 type、seq 和 tag。八种帧均携带 seq，在线格式中固定占首字节之后的 4 B；各消息体不再重复定义 seq。不把 ACK、length、身份或会话 nonce 放进公共头。
- P1 tag 是 4 B 截断 HMAC-SHA256，覆盖实际编码的首字节、公共 seq 和消息体；P0 不发送 tag。接收侧按本地 profile 决定 tag 长度，不根据未经认证的 SYNC profile 降级。
- LR2021 提供物理前导、同步字、长度和 CRC。它们不重复放进软件帧。多字节整数均为小端。

数据模型由一个帧对象组成：type 后接各类型消息体的 union，公共 seq 和 tag 属于外层。仅相同字段布局共用消息体：DATA_UP/DATA_DOWN 共用 data，PAIR_RESP/PAIR_CFM 共用 pair，PAIR_REQ 单独定义。

| 层级 | 字段与约束 |
| --- | --- |
| 帧对象 | type:3 bit、body:union、seq:u32、tag:内嵌 4 B 数组 |
| data | ack:1 bit、data_seq:7 bit、RSSI:u8、LQ:u8、内嵌数据数组容量 503 B |
| sync | profile:1 bit、version:4 bit、hop:u8、nonce_t:u32、frame_index:u32、band_plan |
| pair_req | dev_a:4 B、nonce_a:u32 |
| pair | dev_a:4 B、dev_b:4 B、nonce_a:u32、nonce_b:u32 |
| sess_resp | nonce_t:u32、nonce_r:u32 |
| sess_cfm | nonce_r:u32、hop:u8、frame_index:u32、band_plan |

消息体不得重复公共 seq，公共字段不属于消息体 union。DATA 和 tag 拥有字节数组，不使用数据指针或 size 字段。有效 DATA 长度从整帧长度推导。

数据模型不定义编译器 ABI。编码须明确处理 bit 和小端整数，仅发送所选消息体的有效字段与 DATA 有效字节，不发送对齐填充或未用数组容量。收发接口在帧对象之外传递整帧长度。

## 2. 确定的线格式与长度

所有帧都有 5 B 公共前缀：首字节(1) + seq(4)。下表消息体不包含这个前缀；所有 nonce/frame_index 为 u32，hop 为 u8。P0/P1 总长均是 LR2021 的软件载荷长度，不含物理前导、同步字、物理长度头或 CRC。

| type | 值 | 公共前缀后的消息体，按编码顺序 | P0 总长 | P1 总长 |
| --- | --- | --- | ---: | ---: |
| DATA_UP | 0 | ack_seq(1), RSSI(1), LQ(1), data(N) | 8+N | 12+N |
| DATA_DOWN | 1 | 同上 | 8+N | 12+N |
| SYNC | 2 | hop(1), nonce_t(4), frame_index(4), band_plan(5)；profile/version 已在首字节 | 19 | 23 |
| PAIR_REQ | 3 | dev_a(4), nonce_a(4) | 13 | 17 |
| PAIR_RESP | 4 | dev_a(4), dev_b(4), nonce_a(4), nonce_b(4) | 21 | 25 |
| PAIR_CFM | 5 | 同上 | 21 | 25 |
| SESS_RESP | 6 | nonce_t(4), nonce_r(4) | 13 | 17 |
| SESS_CFM | 7 | nonce_r(4), hop(1), frame_index(4), band_plan(5) | 19 | 23 |

首字节 SYNC 示例：version=11、P1、type=2，编码为 `0xBA`。非 SYNC 首字节就是编号 0–7。

DATA 的 ack_seq 仍为 bit0=ACK、bit1–7=data_seq。DATA 有效长度：

```text
data_len = rx_total_len - 1(type) - 4(seq) - 1(ack_seq) - 1(RSSI) - 1(LQ) - tag_len
         = P1: rx_total_len - 12
         = P0: rx_total_len - 8
```

P1 最大 DATA=499 B，P0 最大 DATA=503 B，两个满帧都恰好 511 B。无用户数据时 N=0，使用同一 DATA 消息返回 ACK/质量，不新增 ACK 类型或结束位置字段。所有最短 P0/P1 包均不小于 6 B，满足 [LR2021 datasheet Rev.1.1 §18.2.2、§18.5 的可变长度 FLRC 6–511 B 约束](https://www.mouser.com/pdfDocs/61979758LR2021_V1_1_datasheet.pdf#page=201)。

DATA 按当前可发送字节数取包，不等凑满、不填充。满载自然发送最大包。固定时隙继续按 511 B 最大物理载荷安排；短包减少 airtime，不提前下一时隙。每次请求按实际帧长计算 airtime 和 duty 消耗。

## 3. 字段职责与计数规则

### 三种计数不可混用

- seq：所有帧共有的发送尝试序号，每方向、每轮配对/会话递增；该轮各类型共用一个发送计数器，不各自计数。重传也取新 seq；供当前接收上下文内的新旧判断和已确认会话的接收质量统计。
- data_seq：每方向单个未确认 DATA 的序号，mod128；重传保持它不变。接收 sink 接受后才推进 ACK。反向 ACK 是下一期望 DATA 序号的奇偶位，初始为 0。
- frame_index：共同 TDD 周期编号；无包时仍推进。供 SYNC/SESS_CFM 同步计划，不放进 DATA。

每方向最多保留一个未确认 DATA。只有接收端接受整包后才推进 ACK；背压须保留原包，重复 DATA 不得再次交付。关闭 ARQ 时仍采用相同 DATA 线格式。

包序号采用 **32 bit、同一轮内严格递增**。不得用有符号差值或环形半空间比较代替新旧判断：环形回绕可能把足够旧的包当作新包，使旧 ACK 错误确认待发送数据。DATA 序号的 mod128 回绕与包序号生命周期分开。
seq 在每轮从 1 起，0 表示尚无接收基线，不上空中。同一轮会话从初始 SYNC、SESS_RESP、SESS_CFM 到后续 DATA/SYNC，各方向的计数连续，不能在确认密钥或进入 CONNECTED 时重新归零。同一有效会话中禁止回绕或重用发送序号；耗尽前以新 nonce 建立新会话，耗尽时不得继续以原会话密钥发送。配对轮次也不得回绕；新的轮次必须有新的握手上下文，不能只清零计数。32 bit 是延长计数空间，不是替代这些检查；它也不改变原 4 B tag 的认证强度边界。这里采用独立协议推理；[RFC4303 §3.3.3](https://www.rfc-editor.org/rfc/rfc4303.html#section-3.3.3) 同样要求防重放启用时不得在同一安全关联中循环序号，本协议不声称实现了 ESP。

seq 是初级的新旧判断，nonce 进一步核对握手轮次，tag 验证消息真实性。接收上下文已确定时，可以先只读检查 seq，快速拒绝旧包；需要识别新轮次时，必须由身份/nonce 和接收状态确定对应上下文，再比较该上下文的 seq。配对、候选握手和已确认会话的接收基线必须区分，不能直接用活跃 DATA 的接收基线判死对端重启后的新握手，也不能因握手包序号更大就替换活跃会话。

仅在类型方向、当前身份/nonce、P1 认证及该上下文的 seq 大于接收基线等检查均通过后，才更新接收基线并处理 ACK、质量、同步和字节交付。初筛失败或后续校验失败均不能推进基线；伪造的大 seq 不能锁死后续合法包。P0 仍无认证，不提供主动攻击防护。DATA_SEQ 的 mod128 回绕可以正常进行，包 seq 回绕则不允许。

首次配对请求/初始 SYNC 仍可能是录下来的旧请求；有公共 seq 也不能在接收基线已丢失后证明其新鲜。响应方的新 nonce 和后续确认负责阻止旧握手完成；同一轮的重复 PAIR/SESS_RESP 只幂等推进待处理握手，不能重复安装密钥或重置已确认进度。nonce 须来自能报告失败的安全随机源，失败时不得发送握手证明或使用时间、计数器替代。零 nonce 有效，但新一轮的本地 nonce 不得重复上一轮。32 bit 随机量具有碰撞概率，不能当作永不重复的会话标识。

LQ 使用已确认会话的有效 DATA、同会话 SYNC 和有效 SESS_CFM；首次进入会话建立基线。所有帧都有公共 seq 不代表配对/候选握手也计入 DATA LQ：PAIR/SESS_RESP 只在相应握手阶段发送，这些阶段的计数不混入已确认会话的差值分母。有效重复确认不能被错算成 LQ 丢包。发送准备/重传和计数分配规则必须保证已提交发送的序号不被重用。

LQ 按至少 128 次预期发送尝试形成窗口，跨窗口连续跟踪序号；128/128 为 100%，127/128 为 99%。

### 配对字段

PAIR_REQ 只发送 dev_a/nonce_a。响应方以 `(dev_a, nonce_a)` 识别一次请求：同一请求重复时复用 nonce_b；首次请求或 ID/nonce 任一变化时产生新 nonce_b，不使用“nonce=0”充当未初始化标志。

PAIR_RESP 的四字段分别用于匹配发起方身份/当前请求，返回响应方身份/nonce；PAIR_CFM 的四字段用于绑定并确认完整待配对元组，收到重复确认只幂等确认同一元组。相同 ID 的两端不建立需要区分方向的配对。

保留 RESP/CFM 的完整回显，继续使用 root 派生 auth_key 对显式元组认证。把它们进一步删成更短消息需要同时改变 MAC 上下文或候选密钥证明；本格式不暗中引入依赖双方隐式状态的认证。

### 会话响应与同步

SESS_RESP 的 nonce_t 绑定当前发起请求，nonce_r 提供对端的新鲜随机量。hop/frame_index 由 TX 在 SYNC/SESS_CFM 提供，正常三步握手不需要从响应方返回。

**统一保留完整三步会话握手，包括 P0 调试格式。** RX 只有收到有效 SESS_CFM 后才能进入 CONNECTED。TX 用 SYNC/SESS_CFM 提供 hop/frame，SESS_RESP 不承担反向同步。不得提供省略确认且复用配对密钥的握手路径。固定频道、RAW、调试日志和 ARQ 隔离不需要这条分支。

响应只有匹配正在等待的 nonce_t 才能建立候选会话。已连接时收到不同 nonce_r，不得直接采用旧响应并重置计数/ARQ：如需要重连，TX 先产生新的 nonce_t，走完整确认。重复消息不重复生成候选随机量，不重置已确认进度。

hop_index 定为 8 bit：实际生成的 FCC915/EU868/CN470/CN433/ISM2400 表长为 250/247/192/255/237，最大索引为 249/246/191/254/236。所有现有区域均可容纳，生成规则上限 256 条也能容纳索引 255。接收时须小于本域 sequence_count，不能将非法索引取模后掩盖错误。

frame_index 保留 32 bit，使长时间运行与 periodic-SYNC 计划继续使用同一 TDD 周期计数。它不承担包防重放职责。

### RSSI/LQ

RSSI 用负 dBm 的绝对值编码为 1–255，0 表示不可用；LQ 为整数百分比 0–100。两者表示发送反馈这一端接收对端的观测，供对端调功使用。

继续各占 1 B，保留 1 dB / 1% 精度及 RSSI=0 的 unavailable 值。101 个 LQ 状态与 256 个 RSSI 编码联合需要至少 15 bit，不能无损放入 8 bit；量化会改变现有 -95/-70 dBm 和 98/99/100% 附近的调功判断及诊断。没有新增 SNR、电池电压或温度空中字段。

## 4. 认证

PAIR 三种帧用 auth_key；SYNC/SESS_RESP 用按身份区分方向的 link_auth；SESS_CFM 和 DATA 用候选/已确认方向会话密钥。候选密钥按双方 nonce 和有序发送/接收身份推导。密钥本身不传输。

## 5. 时隙、跳频与区域约束

Each frame contains two slots: DATA A (0, TX → RX) and DATA B (1, RX → TX). The LR2021 uses hardware sync word 1 for DATA and 2 for pairing. Pairing uses its own sync slot; connected traffic uses the DATA sync slot. A selected frequency band supplies its channel table and modem timing. FHSS derives a deterministic hop sequence from pairing material. The sub-G band uses its board SKU and region's channel, TX-power and duty rules; when the duty budget is exhausted, transmission is deferred. The active TX power never exceeds the configured/regional ceiling. DATA uses one selected band at a time.

固定时隙中的按需变长发送遵守第 2 节。例如 20 B 用户数据形成 32 B P1 帧；短包降低空中时间和占空比消耗，不提高固定包率或提前下一时隙。

The default 2.4 GHz preparation reservation is 2.5 ms. It includes the previous RX window's cleanup, authentication, radio configuration and FIFO loading before the timer's fixed TX launch. The frame period is derived from this reservation and the maximum packet airtime, so both peers must use the same timing configuration. Increasing this reservation trades nominal slot rate for fewer aborted preparations; it does not allow packets to transmit late.

FLRC uses a 24-bit preamble, a 32-bit hardware sync word and a 16-bit physical CRC with coding rate 3/4. DATA matches the full sync word derived from pairing material; pairing uses `ZLRS`. Full sync matching limits noise packets reaching the CRC decoder. Packet airtime and the slot plan are computed from these same modem settings.

Periodic SYNC defaults to 5 seconds, converted to a whole number of actual frames. With the default 2.4 GHz timing this is 450 frames (4.991 seconds). The frame count is 16 bits and includes debug pacing when deriving the interval. Each DATA slot advances the hop index even if idle or skipped. The 2.4 GHz table contains three 79-channel blocks (237 entries); CN433 contains 51 five-channel blocks (255 entries). Each block visits every channel once and reserves its first position for the synchronization channel; the other positions are shuffled from the pairing seed.

RX follows authenticated packet timestamps. When receptions stop, it scans the frame phase with a step derived from the actual frame length and SYNC interval: approximately one third of a frame between SYNC opportunities. The default steps are 8 us per frame on 2.4 GHz and 155 us on CN433. This lets a searching receiver sample different phases of a quiet peer's periodic SYNC. After acquisition, scanning waits through two SYNC intervals without an authenticated reception.

对端重启并开始新的会话握手时，接收端须重新采集相位锁定样本；上一轮已锁定的异常值门限不能阻止新时钟相位被采纳，即使停收时间尚未达到扫描期限。相位重新锁定仍依据合法接收时间戳，不改变固定发射边界或允许晚发。重连验收须检查两个方向实际恢复 DATA 交付，不能仅以 CONNECTED 状态或收到单向确认帧作为成功依据。

### 自动跨频段切换

频段模式有固定 `2.4`、固定 `subg` 和 `auto`。新设备默认 `auto`；已有明确保存的固定模式继续有效，启用自动切换须在两端选择 `auto` 并重启。自动模式优先使用 2.4 GHz，SubG 使用双方一致且适配板载 SKU 的区域配置。一次只运行一个 DATA 频段；探测、切换和失联搜索都属于同一自动切换功能。配对在 2.4 GHz 完成，配对期间不搜索其他频段。

TX 是切换计划的唯一发布方。SYNC 与 SESS_CFM 各增加 5 B `band_plan`，其余六种帧及 DATA 容量不变。第一个字节 bit0–2 是当前信道域（0=FCC915、1=EU868、2=CN470、3=CN433、4=ISM2400），bit3 表示自动模式，bit4 表示目标频段（0=2.4 GHz、1=SubG），bit5–7 必须为零；后四字节为小端 u32 `switch_frame`。零表示没有计划，非零表示在该共同帧开始前切换。目标不同于当前频段，计划必须在当前帧之后且不超过 65535 帧；固定模式不得发布计划。SYNC/确认的域和模式必须匹配当前接收配置，认证、nonce 和 seq 检查全部通过后才能采纳计划。旧帧、错误域、非法或已过期的计划不能改变频段、时钟或接收基线。

正常切换提前至少一个周期同步间隔加 2 秒公告；公告首先覆盖双方已知的周期同步窗口，随后在同步频道重复。接收方收到计划后也在同步频道收发，继续处理 DATA 与 ACK。双方完成 `switch_frame-1` 的两个时隙后，在下一帧边界前停止旧无线事务，取消预装帧，换用目标频段的调制、跳频表和功率上限。过渡边界增加共同的 12 ms 保护间隔，须在最后一个旧帧结束前预留；旧时隙仍按原期限收尾，不能把保护间隔用于延长旧 DATA 发射。新频段重新采集相位，固定发射时刻仍由硬件定时器执行。跳频从新表起点重新对齐；共同 frame_index、发送 seq、接收防重放基线、会话密钥、双向 DATA 序号、接收去重状态、未确认包和串口队列全部保留。旧频段的质量样本与调功观察清除，目标频段从允许的功率上限开始。

2.4 GHz 上任一方向的有效反馈持续 5 秒低于 95% LQ 或 RSSI 低于 -95 dBm 时，TX 安排切至 SubG。单个异常样本不触发切换。SubG 至少稳定运行 60 秒后，TX 安排一次 2.4 GHz 探测；探测须有连续 5 秒的新鲜双向反馈才保留 2.4 GHz，失败则返回 SubG，并重新开始 60 秒等待。这些阈值是初始产品策略，需要后续 RF 校准；瞬时管理暂停不能成为持续质量恶化的证据。

完全失联无法依赖公告成功。默认连续 1.5 秒没有已确认会话的有效对端接收后进入恢复搜索；有效会话在搜索期间保留，不能因搜索重置 ARQ。先尝试另一频段，随后 TX 每个频段停留 20 秒，RX 每个频段停留 4 秒，避免双方同时换频而永久错过。恢复期间 TX 周期发送同步，RX 在同步频道扫描相位；同会话合法 SYNC 恢复共同 frame/hop，捕获期间双方暂停用户 DATA 发射，只收发同步与会话握手，未确认数据保持原状态。接收方收到合法同步/确认后继续在同步频道跟随时钟；认证帧的相位误差收敛到 100 us 以内才结束捕获并回传 ACK/质量，TX 以这份有效反向 DATA 完成捕获。短 ACK 可被接收并不能代替整包 DATA 所需的相位收敛。尚未收到合法同步时使用快速相位扫描；已有合法同步后，至少等待两次握手同步间隔才继续扫描，避免扫描抵消正常相位校正。自动模式下，已完成相位捕获的接收方对每个合法同会话 SYNC 都在紧随其后的反向时隙用同步频道发送 ACK/质量帧，使仅一端进入捕获的情况也能完成双向确认；无需新增控制 ACK 或重置 DATA 去重状态。丢失全部公告、单向丢包、不同启动时刻或一端重启均须通过这种搜索再次汇合。对端重启仍执行完整的新会话握手；单纯频段切换不建立新会话。

SubG 的占空比预算跨切换保留；探测、同步、确认和 DATA 都计入实际 airtime，换出再换回不能重置预算。受占空比限制的区域将无反馈等待与搜索停留时间延长到至少一个预算窗口加 2 秒，以免把合法等待当作链路故障，也给双方留下合法汇合机会。管理暂停超过无反馈期限，或暂停时存在切换计划，则取消未执行计划并重新捕获；没有计划的短暂停留保留原频段与已知时钟，不强制重新捕获，避免普通状态读取触发故障换频。管理期间已接收的 DATA 也须保留，USB 输入容量满时施加背压，恢复后继续交付。所有队列保持背压，不因切换或维护暂停清空数据；无流控 UART 的溢出另行计数。没有可用频段时持续有限停留搜索，管理与看门狗保持可用。

## 6. 管理状态

Management version `4` uses a fixed 90-byte `STATUS_RESP`, filling one 96-byte management frame. Multibyte integers are little endian; RSSI and temperature are signed 16-bit values, and power values are signed 8-bit dBm. The firmware and host use this one status layout.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Management version, role, state, flags；bit3 当前 SubG、bit4 自动模式、bit5 捕获中、bit6 有待执行切换计划 |
| 4 | 8 | Device ID, peer ID |
| 12 | 4 | Local LQ, sub-G hardware SKU, region, configured TX ceiling |
| 16 | 4 | Saved DATA UART baud |
| 20 | 1 | Active security profile |
| 21 | 4 | Local RSSI, peer RSSI; zero means unavailable |
| 25 | 3 | Peer LQ, active TX setpoint, effective power floor |
| 28 | 2 | MCU die temperature in 0.01 °C; `INT16_MIN` means unavailable |
| 30 | 32 | Eight `u32` counters: TX frames, RX frames, CRC errors, authentication errors, ARQ retries, dropped DATA bytes, missed slots, radio failures |
| 62 | 4 | FHSS snapshot frequency in Hz |
| 66 | 2 | FHSS index |
| 68 | 4 | Frame period in microseconds |
| 72 | 2 | LR2021 junction temperature in 0.01 °C |
| 74 | 2 | LR2021 temperature near XOSC in 0.01 °C |
| 76 | 2 | LR2021 VBAT supply in millivolts |
| 78 | 2 | LR2021 version major, minor |
| 80 | 3 | Chip mode, last command status, reset source |
| 83 | 1 | Validity bits: bit0 junction temperature, bit1 XOSC temperature, bit2 VBAT, bit3 version, bit4 mode/command/reset/IRQ, bit5 chip errors |
| 84 | 2 | LR2021 latched chip error mask |
| 86 | 4 | LR2021 IRQ mask at the maintenance boundary |

The frequency is the hopping schedule's current channel; periodic SYNC uses the fixed synchronization channel. MCU temperature is the internal die sensor reading, not ambient or LR2021 temperature. FLRC does not supply a LoRa SNR measurement.

LR2021 measurements run only on the RF owner at boot or during a paused management session. The mode and IRQ are captured after RF has stopped at the maintenance boundary, so standby XOSC is expected; these are not the preceding live RX/TX state. Sensor reads use the chip's unit format at 13-bit ADC resolution; temperatures are signed Q8.5 converted to hundredths of a degree. Validity bits distinguish unavailable values from valid zero readings. Reads do not clear the chip's error/IRQ/reset information. The debug shell reads the same cached observation via the RF zbus channel and prints its age; it does not issue SPI or pause RF to refresh it. Internal sensor readings are not external ambient temperature or calibrated battery-pack voltage. See [LR2021 datasheet Rev. 1.1, sections 6.5 and 6.7](https://www.mouser.com/pdfDocs/61979758LR2021_V1_1_datasheet.pdf).

## 7. 验收

验收须覆盖以下场景：

- 八类型独立字节向量、每种帧都有且仅有一个公共 u32 seq、精确固定长度、SYNC 首字节 bit 映射、P0/P1 DATA 长度边界和 511 B 满帧；DATA 数组所有权和 tag 位置。
- 32 bit seq 跨 32768/65536 边界、旧包/旧 ACK 不更改队列、重复包/确认、序号耗尽不得回绕。旧密钥包与错误角色类型不能驱动状态或 ACK。
- DATA 127→0、多轮回绕、丢 ACK、sink 背压、准备帧旧 ACK 快照以及双向不同速度。
- 配对首次 nonce_a=0、重复请求、相同 nonce_a/不同 dev_a、错误 ID/nonce 确认、落盘与重启；保留真实 root/身份资料。
- 会话响应丢失/重复、旧响应触发不得直接切换会话、确认丢失/重复、对端重启、nonce/frame/hop 同步。普通会话内重传不能靠重启会话解决；不把未收到 ACK 的字节当作已交付。
- 所有类型在对应接收上下文内校验 seq；握手到 CONNECTED 不回退该轮发送序号，新轮次建立正确基线；seq 初筛只读，错误 nonce/认证和伪造大 seq 均不推进基线或驱动状态。
- 熵源不可用、有限等待后超时或随机服务报错时，两种角色均不能发送依赖新 nonce 的握手；失败输出不能成为 nonce，不能退回时钟或普通伪随机数。恢复熵源后须能完成新的握手，保留待交付 DATA。
- LQ 窗口及有效确认的记账、所有区域索引边界、两频段可变长度短包/满包实机、FIFO tail/underflow、固定发射时刻和关键 SRAM 函数。
- 自动模式：双向质量恶化、完全/单向失联、公告丢失、切换时未确认 DATA 与丢 ACK、背压、探测成功/失败、短时异常、不同启动时刻、对端重启和管理暂停。固定模式不得自动切换；错误认证/seq/nonce/域/模式或非法计划不得触发切换。占空比不得借切换重置，恢复须验证双向实际字节交付及无重复。

seq、ACK、长度、握手和格式的边界分别检查；字段省略不得降低身份/请求绑定或将重发当新用户字节。验证结果与运行记录须独立于设计文档保存。
