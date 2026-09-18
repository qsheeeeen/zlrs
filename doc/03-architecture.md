# 系统架构

本文写系统怎么搭起来：两条链路怎么分工、频段角色怎么配、分时怎么排、时间预算怎么算、代码怎么分层、线程与回调怎么约定。平台与固件见 [02-platform](02-platform.md)，空中协议见 [04-protocol](04-protocol.md)，芯片数据见 [06-radio](06-radio.md)。

## 1. 两条链路

| 链路 | 默认频段 | 调制 | 承载 | 优化目标 |
| --- | --- | --- | --- | --- |
| 控制链路 | sub-GHz | FLRC | CRSF（通道下行 + 遥测上行） | 距离与稳定性 |
| 数据链路 | 2.4 GHz | FLRC | 串口透传字节流 | 帧率与吞吐 |

两条链路各跑一份相同的状态机，**互相独立**：一条断了不影响另一条。但要注意它们共用一颗射频、一个 MCU、一路供电——**这不是真正的冗余链路**，只是在干扰层面互为备份。射频芯片或电源故障会让两条一起失效。

不做频段自动故障切换：切换判据需要真实干扰数据才能标定，而两条链路状态独立已经覆盖了干扰层面的备份需求。

## 2. 频段角色可配

目标是把频段与链路角色的对应关系做成配置项，而不是写死：

| 模式 | 控制链路 | 数据链路 |
| --- | --- | --- |
| **远距模式**（默认） | sub-GHz（2 km） | 2.4G（近距离内可用） |
| **竞速模式** | 2.4G 独占（高帧率、低延迟） | 让出时隙，或关闭 |

这一条来自"一套方案覆盖所有载体"：远距要 sub-GHz 的灵敏度和覆盖，而 FPV 竞速要 2.4G 的短空口时间。两者对频段角色的要求正好相反。

实现上几乎没有额外代价——同步字的派生标签本来就是按**角色**命名（`sync/control` / `sync/data`），不是按频段命名，所以角色互换不影响密钥结构（[05-security](05-security.md) 第 3 节）。

两种模式都是 **`CONFIG_ZLRS_APP_BAND_ROLE_*`** 选出来的编译期配置（不是运行时可切换项，配置协议里没有这一项）：

- **远距模式**（默认，`..._SUBG_CONTROL=y`）：控制链路跑本地区域的 sub-G 频段（由 SKU 与地区表决定，见 `lib/region`），数据链路跑 2.4G。控制链路吃法规门（功率/速率上限、占空比）。
- **竞速模式**（`..._SUBG_DATA=y`）：控制链路独占 2.4G（`CONFIG_ZLRS_APP_RACING_CONTROL_RATE_BPS/_HZ`，默认 2.6 Mbps / 500 Hz），数据链路让到 sub-G 并受地区法规约束；不想留数据链路就直接 `CONFIG_ZLRS_APP_LINK_DATA=n`。法规门只作用在 sub-G 那条链路上——2.4G 没有地区表。

超帧周期在两边的算法一样：数据链路的窗口按 `data_every_n` 摊进每一帧，控制帧率被它拖下来多少由时间计划算出来（例如 2.4G 控制 500 Hz + sub-G 数据 0.325 Mbps 的 128 B 帧 → 420 Hz，见主机侧用例 `region: racing plan`）。

## 3. 分时与射频调度

分时不自己写调度器，而是把两条链路注册成两个 RAC 客户端，用优先级让 radio planner 仲裁：

| 链路 | RAC 优先级 | 调度方式 |
| --- | --- | --- |
| 控制链路 | `RAC_HIGH_PRIORITY` | 超帧由应用排序，RAC 侧 `ASAP` |
| 数据链路 | `RAC_MEDIUM_PRIORITY` | 控制链路空出来的时隙，`ASAP` |

时隙顺序由应用保证：RAC 的排期粒度是毫秒，而超帧时隙是亚毫秒级，所以应用按超帧推进、逐个提交事务，RAC 只负责尽快执行。超帧的具体形状见 [04-protocol](04-protocol.md) 第 4 节；跳频见同文第 5 节。

两条链路默认都用 FLRC。改调制类型（FLRC ↔ LoRa）在射频侧比单纯改频率贵得多，共用调制能让每次切换只改频率。LoRa 只作远距兜底档。

调制是**按链路各自选**的（`CONFIG_ZLRS_APP_CONTROL_MODULATION_*`）：窄带地区（868 / 470 / 433）放不下宽带 FLRC 时，控制链路改用 LoRa（默认 SF7 / BW125、配置 10 Hz，实际由空中时间定），数据链路仍在 2.4G 跑 FLRC；超帧周期由最慢的那条链路决定，时隙预算、ARQ 超时这些常数跟着档位派生。见 [04-protocol](04-protocol.md) 4.1.1。

## 4. 时序预算与速率档

取自 LR2021 datasheet Rev 1.1 Table 3-23（typical 值）。这些数是射频栈内部的时序，用来估算时隙余量；完整表见 [06-radio](06-radio.md)。

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

粗算：6.67 ms 周期里分 40% 给 2.4G 数据链路 ≈ 650 字节/周期 ≈ **97 kB/s**。完整速率与灵敏度表见 [06-radio](06-radio.md)。

## 5. 协议设计要点

帧格式、包类型、链路状态机与分时结构见 [04-protocol](04-protocol.md)。身份与密钥见 [05-security](05-security.md)。

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
lib/led/              状态灯模式引擎：状态 → 颜色与节奏
lib/usb/              USB 设备栈：usbd context + devicetree 里的全部 CDC
app/                  Zephyr 应用：线程与超帧、端口层（CRSF 端口、数据口透传、
                      配置模式、端点选择）、射频移植、按键与持久化
tests/host/           主机侧单测（不依赖 Zephyr）
doc/                  设计文档
```

分层原则：协议核心与链路层都能在主机上编译测试（同一个 `lib/`，同一份源码）；`lib/link/` 只依赖射频抽象接口，`app/` 才依赖 Zephyr 与 USP。射频侧的一切——SPI 时序、校准、频段选择、收发调度——都在 `usp_zephyr` 里，本仓库不含射频驱动。跨层错误处理约定见 [02-platform](02-platform.md) 第 6 节。

发射前还要过一道**地区门**：`lib/region` 给出该地区的信道表、功率上限与占空比上限（EU868 是 1%），额度用完的时隙就让出去，次数计入日志。见 [04-protocol](04-protocol.md) 5.2。

## 7. 线程模型与回调契约

构建环境见 [README](../README.md)：`scripts/setup-zephyr.sh` 用本仓库的 `west.yml` 建工作区，`scripts/zephyr-env.sh` 进环境并指向 Zephyr SDK。

### 7.1 线程划分

| 线程 | 优先级 | 职责 |
| --- | --- | --- |
| RAC 线程（USP 提供） | `-4` | 跑 radio planner，派发收发回调 |
| 链路线程（`main`） | `-2` | 链路状态机、组装/提交事务、处理结果 |
| 串口 | 中断上下文 | 环形缓冲收发，不做射频相关决策 |

采用协作式多线程（`CONFIG_USP_MAIN_THREAD=y`、`CONFIG_USP_THREADS_MUTEXES=n`，优先级见 `app/prj.conf`）：RAC API 直调，没有互斥锁开销；链路逻辑全是 I/O 驱动、会频繁让出。代价是串口突发处理太久会推迟射频事件，所以串口收发切成小块并及时让出。USP 的另外两种线程模式与 `SMTC_SW_PLATFORM()` 的展开见 usp_zephyr 的 `doc/THREAD_MANAGEMENT.md`。

**配置模式不在这套线程里**：它在 `main()` 里阻塞跑（数据口轮询收发，不跑超帧、不做透传），退出后继续正常启动。三个入口的语义见 [02-platform](02-platform.md) 第 2 节。

### 7.2 回调契约

`callback_post_radio_transaction` 在 RAC 线程上下文里执行，只允许：

1. 记录 `rp_status_t` 与收到的长度；
2. 把结果投递给链路线程（`k_sem` / `k_msgq`）；
3. 返回。

不在回调里做校验、不在回调里翻协议栈、不在回调里碰串口，**也不在回调里提交下一个事务**：抢占式 + 互斥锁模式下那是对 `rac_api_mutex` 重复加锁，属于官方示例未覆盖的路径。

### 7.3 与 USP 的集成约束

RAC 的初始化由应用负责：`SMTC_SW_PLATFORM_INIT()` 只等 USP 线程就绪，它不会调用 `smtc_rac_init()`——官方示例里是应用自己调，照做即可。日志模块从 USP v1.1.2 起由 `usp_zephyr` 自己注册，应用侧不用再补 `LOG_MODULE_REGISTER(usp, ...)`。

## 8. 诊断与可观测性

诊断**不另建通道**：链路质量走 CRSF 既有的链路统计帧，见 [04-protocol](04-protocol.md) 2.7。数据链路的溢出与丢弃字节数没有现成的 CRSF 字段承载，不上报。
