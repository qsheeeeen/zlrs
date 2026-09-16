# zlrs 系统设计

基于 Zephyr 自研的低延迟遥控 + 串口透传链路，射频芯片为 Semtech LR2021。

## 1. 目标与边界

**定位**：开源链路项目，所有人都能自己 DIY 硬件与固件。

**服务对象**：

- 固定翼、多旋翼、车、船——一套方案覆盖
- 严肃 DIY 用户
- 工业与数控场景是**长远目标**，当前不为其做专项设计，但架构不能挡住它

**为什么自研**：可控、可扩展（现成方案难以适配将来的其它项目）、以及信息安全（见 [02-security.md](02-security.md)）。

**硬件取向**：

- 单颗 LR2021 分时双频
- 不做 PWM，一切走协议
- TX 与 RX 用同一套硬件逻辑，角色可配
- 一份固件适配多种硬件配置，USB 之类的特性可编译裁掉

**明确不做**：

- 不与官方 ExpressLRS 逐字节兼容
- 不追求 20 km 级距离（那是 4G 的场景），**控制链路目标 2 km**
- 不做图传
- 不防 L3 级攻击者（见 [02-security.md](02-security.md) 第 7 节）
- 不做产线，没有出厂密钥注入
- 配置协议、NVS 布局与地区配置表的细节不在本文档范围内

**性能目标**：

| 项 | 目标 |
| --- | --- |
| 控制距离 | 2 km 稳定 |
| 控制延迟 | 支持四旋翼，目标逼近 FPV 竞速级别 |
| 数据链路 | 允许降速换取距离，不承诺延迟 |

长期质量要求：**稳定、确定性延迟、故障可预测可检测、长期运行不退化**。这三条不做专项验证，但所有设计选择都要经得起它们。

**技术前提**：LR2021 是半双工、单 switchless 前端，同一时刻只能在一个频段工作，但频段切换开销很小（见第 4 节）。射频栈不自己写，用 Semtech 官方 [usp_zephyr](https://github.com/Lora-net/usp_zephyr) 的 RAC 层。

## 2. 硬件与软件基线

| 项目 | 选择 |
| --- | --- |
| 射频 | Semtech LR2021（QFN32），SPI + BUSY + DIO |
| 参考硬件 | 微雪 RP2350-Zero（RP2350A，4 MB flash）+ 微雪 Core2021（LR2021）。sub-G 是两个 SKU：HF 850–930M（868/915）、LF 410–510M（470/433），板级做成 hf / lf 两个变体 |
| 板级定义 | 本仓库 `boards/zlrs/rp2350_zero_core2021/`，接线、flash 分区与两个 SKU 的标定表都在这里 |
| 射频栈 | `usp_zephyr` + `usp` v1.1.2-feature-202604，使用 RAC/RALF 层 |
| Zephyr | v4.4.0（RP2350 的 TRNG 驱动从这一版起在树内） |
| 仓库拓扑 | T2，zlrs 自己当 manifest 仓库并存放应用 |

不用 LBM（LoRa Basics Modem）——那是 LoRaWAN 那套；我们要的是裸包收发，即 RAC（Radio Access）层。相关能力在 `smtc_rac_flrc.c` / `smtc_rac_fsk.c` / `smtc_rac_lora.c`，示例见 `samples/usp/sdk/packet_error_rate_flrc`。

### 2.1 端口与工作模式

**TX 与 RX 是同一套硬件、同一份固件**，差别只在角色配置。

| 端口 | 正常模式 | 配置模式 |
| --- | --- | --- |
| CRSF UART | CRSF | 不变 |
| 数据 UART | 透传字节流 | **改跑配置协议** |
| USB CDC（CRSF） | CRSF | 不变 |
| USB CDC（数据） | 透传字节流 | **改跑配置协议** |
| PWM | 不做 | — |

**没有专门的控制口**：配置和透传复用同一条数据通路，靠工作模式区分。

每条链路有两块端点，二选一——主机把该链路的 CDC 打开（DTR 有效）就走 USB，否则走 UART。端口分配：CRSF = UART0（GP0/GP1，`zlrs-crsf-uart`）+ `cdc_acm_uart0`，数据 = UART1（GP8/GP9，`zlrs-data-uart`）+ `cdc_acm_uart1`；控制台在 USB 的第三个 CDC 上（`cdc_acm_uart2`，`zephyr,console`）。别名按端点种类命名（`-uart` / `-cdc`）。

按键的**时机**决定行为：

| 时机 | 行为 |
| --- | --- |
| **开机时按住按键** | 进入配置模式 |
| **开机后按按键** | 配对 |

配置模式在超时（建议 5 分钟）或收到明确的退出指令后结束。

这样做的好处是 TX 和 RX 用**完全相同的流程**，用户只需要记一套操作。代价是 TX 也不能在运行时边透传边配置——要改配置就得重启并按住按键。

轻量硬件可以裁掉 USB，那时配置只能走数据 UART。这是下一节"特性可裁剪"的一个具体例子。

### 2.2 特性可裁剪

一份固件要能适配多种硬件配置，这是硬要求而不是加分项：

- **USB 必须可裁掉**——RX 轻量版没有原生 USB
- 数据 UART 也应是可选的
- 用 Kconfig 与 devicetree 表达能力，应用层靠 DT 探测，**不要到处 `#ifdef`**

本板（RP2350-Zero）每条链路都是 UART + CDC 两块端点，控制台在 USB 第三个 CDC。轻量板把 `zlrs-data-cdc`（或整个 `&usbd`）删掉即可；别名一个都不挂就是 "这条链路的端口不存在"，一样编得过。

### 2.3 上板调试的分层开关

硬件第一次上电时不要一次把所有东西都打开。每一项复杂功能都有自己的 Kconfig 开关，从最简单的形态往上逐层验证：每关掉一层，剩下的还能跑通就说明问题不在被关掉的那一层。关掉的项在开机时会打一条 `bring-up:` 警告，日志里能看出这台设备不是生产形态。

| 开关 | 关掉之后 | 主机侧用例 |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_SECURITY_PROFILE_P0=y` | 帧不带 tag，先把收发时序与帧格式跑通 | `link: bring-up without tag (P0)` |
| `CONFIG_ZLRS_APP_FHSS=n` | 固定发在同步信道，不跳频 | `link: bring-up without hopping` |
| `CONFIG_ZLRS_APP_SESSION_HANDSHAKE=n` | 配对级钥匙直接当会话钥匙：BEACON 之后只回一条 `SESS_RESP` | `link: bring-up without session handshake` |
| `CONFIG_ZLRS_APP_TELEMETRY=n` | 控制链路只剩下行 RC（TX 的失联判定一并关掉，否则它会一直重建会话） | `link: bring-up without telemetry` |
| `CONFIG_ZLRS_APP_DATA_ARQ=n` | 透传回到 best-effort（默认就是关） | `link: arq *`、`link: backpressure` |
| `CONFIG_ZLRS_APP_RATE_ADAPT=n` | 固定速率档（默认就是关） | `link: rate adapt` |
| `CONFIG_ZLRS_APP_DYNAMIC_POWER=n` | 固定发射功率 | `power: *` |
| `CONFIG_ZLRS_APP_RESYNC_PERIOD_MS=0` | 不做连接态周期 `BEACON`：跳频索引只在建链时对齐，对端重启也不会自动重连 | `link: hop resync`、`link: peer restart reconnect` |
| `CONFIG_ZLRS_APP_CONTROL_MODULATION_FLRC=y`（默认） | 控制链路跑 FLRC；换成 `..._LORA=y` 则进 LoRa 远距档（超帧周期跟着变长） | `link: lora control modulation`、`region: lora control plan` |
| `CONFIG_ZLRS_APP_LINK_CONTROL=n` / `ZLRS_APP_LINK_DATA=n` | 只跑一条链路：先把单条链路（含射频时序）跑通，再合起来看分时 | 编译验证（两个 SKU 都在 CI 里） |
| `CONFIG_ZLRS_APP_CRSF_PORT=n` | 不初始化 CRSF 端口：TX 只发配置好的失控值 | 编译验证 |
| `CONFIG_ZLRS_APP_CONFIG_MODE=n` / `ZLRS_APP_PAIRING_BUTTON=n` / `ZLRS_APP_STATUS_LED=n` | 关掉配置模式 / 按键配对 / 状态灯 | 编译验证 |

推荐的顺序：`P0` + 关跳频 + 关握手 + 关遥测 → 先确认"自己发的帧自己能收到"；然后打开 tag（`P1`）；再打开跳频；再打开遥测与失联判定；最后才是速率自适应与动态功率——这两项的阈值本来就要等实测曲线（见 [05-roadmap.md](05-roadmap.md) 的 M4），上板第一步不该被它们干扰。

每个开关都对应一条主机侧用例，断言"关掉之后链路照常工作"：这套用例跑在 `tests/host`，不需要硬件。

## 3. 系统架构

### 3.1 两条链路

| 链路 | 默认频段 | 调制 | 承载 | 优化目标 |
| --- | --- | --- | --- | --- |
| 控制链路 | sub-GHz | FLRC | CRSF（通道下行 + 遥测上行） | 距离与稳定性 |
| 数据链路 | 2.4GHz | FLRC | 串口透传字节流 | 帧率与吞吐 |

两条链路各跑一份相同的状态机，**互相独立**：一条断了不影响另一条。但要注意它们共用一颗射频、一个 MCU、一路供电——**这不是真正的冗余链路**，只是在干扰层面互为备份。射频芯片或电源故障会让两条一起失效。

不做频段自动故障切换：切换判据需要真实干扰数据才能标定，而两条链路状态独立已经覆盖了干扰层面的备份需求。

### 3.2 频段角色可配

**目标**是频段与链路角色可配，而不是写死：

| 模式 | 控制链路 | 数据链路 |
| --- | --- | --- |
| **远距模式** | sub-GHz（2 km） | 2.4G（近距离内可用） |
| **竞速模式** | 2.4G 独占（高帧率、低延迟） | 让出时隙，或关闭 |

这一条来自"一套方案覆盖所有载体"：远距要 sub-GHz 的灵敏度和覆盖，而 FPV 竞速要 2.4G 的短空口时间。两者对频段角色的要求是相反的。

实现上几乎没有额外代价——同步字的派生标签本来就是按**角色**命名（`sync/control` / `sync/data`），不是按频段命名，所以角色互换不影响密钥结构。

**当前只实现了远距模式**：控制链路跑本地区域的 sub-G 频段（由 SKU 与地区表决定，见 `lib/region`），数据链路固定 2.4G。"竞速模式"里让控制链路独占 2.4G 还没有做 ——配置协议里没有这一项，见 [05-roadmap.md](05-roadmap.md) 的长期方向。

### 3.3 分时

分时不自己写调度器，而是把两条链路注册成两个 RAC 客户端，用优先级让 radio planner 仲裁——线程与回调的做法见第 7 节。超帧的具体形状见 [03-protocol.md](03-protocol.md) 第 4 节。跳频采用逐包跳频，见 [03-protocol.md](03-protocol.md) 第 5 节；法规约束同节 5.2。

两条链路默认都用 FLRC。改调制类型（FLRC ↔ LoRa）在射频侧比单纯改频率贵得多，共用调制能让每次切换只改频率。LoRa 只作远距兜底档。

调制是**按链路各自选**的（`CONFIG_ZLRS_APP_CONTROL_MODULATION_*`）：窄带地区（868 / 470 / 433）放不下宽带 FLRC 时，控制链路改用 LoRa（默认 SF7 / BW125、配置 10 Hz，实际由空中时间定），数据链路仍在 2.4G 跑 FLRC；超帧周期由最慢的那条链路决定，时隙预算、ARQ 超时这些常数跟着档位派生。见 [03-protocol.md](03-protocol.md) 4.1.1。

## 4. 时序预算（背景数据）

取自 LR2021 datasheet Rev 1.1 Table 3-23（typical 值）。这些数是射频栈内部的时序，用来估算时隙余量。

| 符号 | 含义 | typ |
| --- | --- | --- |
| `PLL_LOCK` | 合成器锁定 | 32 µs |
| `TSFSTX1` | FS → Tx | 45 µs |
| `TSFSRX2` | FS → Rx（FSK／FLRC） | 37 µs |
| `TSTXRX` | Tx → Rx | 91 µs |
| `TSRXTX` | Rx → Tx | 107 µs |
| `TSXORX` | STDBY_XOSC → Rx | 84 µs |
| `TSXOTX` | STDBY_XOSC → Tx | 76 µs |

一次「sub-GHz 发 → 2.4G 收」的切换约 `Tx→FS(54) + PLL(32) + FS→Rx(37) ≈ 123 µs`，来回约 250 µs。按 150 Hz 控制帧率（周期 6.67 ms）算，切换开销约 3.7%。

收发路径按频率自动分流：`ral_lr20xx_bsp_get_rx_cfg()` 在 `freq >= 1.6 GHz` 时选 HF 通道，否则 LF——所以我们只管填频率。

### 速率档位

| 档位 | FLRC 原始速率 | 有效速率（CR 3/4） | 用途 |
| --- | --- | --- | --- |
| 近距高速 | 2.6 Mbps | 1.95 Mbps | 2.4G 数据链路默认 |
| 均衡 | 1.3 Mbps | 0.975 Mbps | 控制链路默认 |
| 远距 | 0.65 Mbps | 0.488 Mbps | 控制链路降级 |
| 兜底 | 0.325 Mbps | 0.244 Mbps | 控制链路保底 |

粗算：6.67 ms 周期里分 40% 给 2.4G 数据链路 ≈ 650 字节/周期 ≈ **97 kB/s**。完整速率与灵敏度表见 [04-lr2021.md](04-lr2021.md)。

## 5. 协议设计

帧格式、包类型、链路状态机与分时结构见 [03-protocol.md](03-protocol.md)。身份与密钥见 [02-security.md](02-security.md)。

透传数据的两种语义，做成可切换 profile：

- **best-effort**：裸字节流，缓冲满即丢，附溢出计数。延迟最低。
- **ARQ**：长度前缀 + 序列号 + ACK/重传。适合 MAVLink 这类不能丢的数据。

QoS 分档按场景切换 profile，而不是散落在代码里。

## 6. 软件分层

```
include/zlrs/proto/   协议核心头文件
lib/proto/            协议核心：帧编解码、分片重组、FHSS 跳频表、
                      密钥派生与标签。密码学原语在设备侧走 PSA
                      （lib/proto/crypto_psa.c），主机测试另有一份
                      等价的纯 C 实现（tests/host/crypto_soft.c）。
                      空口 CRC 由射频硬件负责
lib/link/             链路层：会话与配对、密钥派生与标签、
                      把帧翻译成 RAC 事务；ARQ profile 与速率自适应策略
lib/crsf/             CRSF 端口编解码：RC 通道帧与链路统计帧
lib/config/           配置协议：帧格式与命令分发（落盘通过回调交给应用）
lib/region/           地区配置层：信道表、功率上限、占空比记账
app/                  Zephyr 应用：线程与超帧、端口层（CRSF 端口、数据口透传、
                      配置模式、端点选择）、射频移植、按键与持久化
tests/host/           主机侧单测（不依赖 Zephyr）
doc/                  设计文档
```

分层原则：协议核心与链路层都能在主机上编译测试（同一个 `lib/`，同一份源码）；`lib/link/` 只依赖射频抽象接口，`app/` 才依赖 Zephyr 与 USP。

射频侧的一切——SPI 时序、校准、频段选择、收发调度——都在 `usp_zephyr` 里，本仓库不含射频驱动。

发射前还要过一道**地区门**：`lib/region` 给出该地区的信道表、功率上限与占空比上限（EU868 是 1%），额度用完的时隙就让出去，次数计入日志。见 [03-protocol.md](03-protocol.md) 5.2。

## 7. 线程模型

构建环境见 [README](../README.md)：`scripts/setup-zephyr.sh` 用本仓库的 `west.yml` 建工作区，`scripts/zephyr-env.sh` 进环境并指向 Zephyr SDK。

### 7.1 线程划分

| 线程 | 优先级 | 职责 |
| --- | --- | --- |
| RAC 线程（USP 提供） | `-4` | 跑 radio planner，派发收发回调 |
| 链路线程（`main`） | `-2` | 链路状态机、组装/提交事务、处理结果 |
| 串口 | 中断上下文 | 环形缓冲收发，不做射频相关决策 |

采用协作式多线程（`CONFIG_USP_MAIN_THREAD=y`、`CONFIG_USP_THREADS_MUTEXES=n`，优先级见 `app/prj.conf`）：RAC API 直调，没有互斥锁开销；链路逻辑全是 I/O 驱动、会频繁让出。代价是串口突发处理太久会推迟射频事件，所以串口收发切成小块并及时让出。USP 的另外两种线程模式与 `SMTC_SW_PLATFORM()` 的展开见 usp_zephyr 的 `doc/THREAD_MANAGEMENT.md`。

**配置模式不在这套线程里**：它在 `main()` 里阻塞跑（数据口轮询收发，不跑超帧、不做透传），退出后继续正常启动。按键的时机把"配置"和"配对"分开，见 2.1。

### 7.2 回调契约

`callback_post_radio_transaction` 在 RAC 线程上下文里执行，只允许：

1. 记录 `rp_status_t` 与收到的长度；
2. 把结果投递给链路线程（`k_sem` / `k_msgq`）；
3. 返回。

不在回调里做校验、不在回调里翻协议栈、不在回调里碰串口，**也不在回调里提交下一个事务**：抢占式 + 互斥锁模式下那是对 `rac_api_mutex` 重复加锁，属于官方示例未覆盖的路径。

### 7.3 与 USP 的集成约束

RAC 的初始化由应用负责：`SMTC_SW_PLATFORM_INIT()` 只等 USP 线程就绪，它不会调用 `smtc_rac_init()`——官方示例里是应用自己调，照做即可。日志模块从 USP v1.1.2 起由 `usp_zephyr` 自己注册，应用侧不用再补 `LOG_MODULE_REGISTER(usp, ...)`。

## 8. 诊断与可观测性

诊断**不另建通道**：链路质量走 CRSF 既有的链路统计帧，见 [03-protocol.md](03-protocol.md) 2.7。数据链路的溢出与丢弃字节数没有现成的 CRSF 字段承载，不上报。
