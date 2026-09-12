# zlrs 系统设计

基于 Zephyr 自研的低延迟遥控 + 串口透传链路，射频芯片为 Semtech LR2021。

## 1. 目标与边界

**定位**：开源链路项目，所有人都能自己 DIY 硬件与固件。

**服务对象**：

- 固定翼、多旋翼、车、船——一套方案覆盖
- 严肃 DIY 用户
- 工业与数控场景是**长远目标**，当前不为其做专项设计，但架构不能挡住它

**为什么自研**：可控、可扩展（现成方案难以适配将来的其它项目）、以及信息安全
（见 [02-security.md](02-security.md)）。

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

长期质量要求：**稳定、确定性延迟、故障可预测可检测、长期运行不退化**。
这三条不做专项验证，但所有设计选择都要经得起它们。

**技术前提**：LR2021 是半双工、单 switchless 前端，同一时刻只能在一个频段工作，
但频段切换开销很小（见第 4 节）。射频栈不自己写，用 Semtech 官方
[usp_zephyr](https://github.com/Lora-net/usp_zephyr) 的 RAC 层。

## 2. 硬件与软件基线

| 项目 | 选择 |
| --- | --- |
| 射频 | Semtech LR2021（QFN32），SPI + BUSY + DIO |
| 参考硬件 | XIAO nRF54L15 + Wio-LR2021（Semtech 官方验证组合，无原生 USB） |
| 射频栈 | `usp_zephyr` v1.0.0 + `usp` v1.0.0，使用 RAC/RALF 层 |
| Zephyr | v4.2.0（与 usp_zephyr 对齐） |
| 仓库拓扑 | T2，zlrs 自己当 manifest 仓库并存放应用 |

不用 LBM（LoRa Basics Modem）——那是 LoRaWAN 那套；我们要的是裸包收发，
即 RAC（Radio Access）层。相关能力在 `smtc_rac_flrc.c` / `smtc_rac_fsk.c` /
`smtc_rac_lora.c`，示例见 `samples/usp/sdk/packet_error_rate_flrc`。

### 2.1 端口与工作模式

**TX 与 RX 是同一套硬件、同一份固件**，差别只在角色配置。

| 端口 | 正常模式 | 配置模式 |
| --- | --- | --- |
| CRSF UART | CRSF | 不变 |
| 数据 UART | 透传字节流 | **改跑配置协议** |
| USB CDC（CRSF） | CRSF | 不变 |
| USB CDC（数据） | 透传字节流 | **改跑配置协议** |
| PWM | 不做 | — |

USB 只有**两个** CDC，与空口上的两条链路一一对应。**没有专门的控制口**——
配置和透传复用同一条数据通路，靠工作模式区分。

按键的**时机**决定行为：

| 时机 | 行为 |
| --- | --- |
| **开机时按住按键** | 进入配置模式 |
| **开机后按按键** | 配对 |

配置模式在超时（建议 5 分钟）或收到明确的退出指令后结束。

这样做的好处是 TX 和 RX 用**完全相同的流程**，用户只需要记一套操作。
代价是 TX 也不能在运行时边透传边配置——要改配置就得重启并按住按键。

轻量硬件可以裁掉 USB，那时配置只能走数据 UART。这是下一节"特性可裁剪"的
一个具体例子。

### 2.2 特性可裁剪

一份固件要能适配多种硬件配置，这是硬要求而不是加分项：

- **USB 必须可裁掉**——RX 轻量版没有原生 USB
- 数据 UART 也应是可选的
- 用 Kconfig 与 devicetree 表达能力，应用层靠 DT 探测，**不要到处 `#ifdef`**

XIAO nRF54L15 没有原生 USB 控制器，所以 USB 只在带原生 USB 的板子上启用。

## 3. 系统架构

### 3.1 两条链路

| 链路 | 默认频段 | 调制 | 承载 | RAC 优先级 | 优化目标 |
| --- | --- | --- | --- | --- | --- |
| 控制链路 | sub-GHz | FLRC | CRSF（通道下行 + 遥测上行） | `HIGH` | 距离与稳定性 |
| 数据链路 | 2.4GHz | FLRC | 串口透传字节流 | `MEDIUM` | 帧率与吞吐 |

两条链路各跑一份相同的状态机，**互相独立**：一条断了不影响另一条。但要注意
它们共用一颗射频、一个 MCU、一路供电——**这不是真正的冗余链路**，只是在干扰
层面互为备份。射频芯片或电源故障会让两条一起失效。

不做频段自动故障切换：切换判据需要真实干扰数据才能标定，而两条链路状态独立
已经覆盖了干扰层面的备份需求。

### 3.2 频段角色可配

**频段与链路角色是配置项，不是写死的。**

| 模式 | 控制链路 | 数据链路 |
| --- | --- | --- |
| **远距模式** | sub-GHz（2 km） | 2.4G（近距离内可用） |
| **竞速模式** | 2.4G 独占（高帧率、低延迟） | 让出时隙，或关闭 |

这一条来自"一套方案覆盖所有载体"：远距要 sub-GHz 的灵敏度和覆盖，
而 FPV 竞速要 2.4G 的短空口时间。两者对频段角色的要求是相反的。

实现上几乎没有额外代价——同步字的派生标签本来就是按**角色**命名
（`sync/control` / `sync/data`），不是按频段命名，所以角色互换不影响密钥结构。

### 3.3 分时

分时不自己写调度器，而是把两条链路注册成两个 RAC 客户端，用优先级让
radio planner 仲裁——详见 [04-threading.md](04-threading.md)。
超帧的具体形状见 [03-protocol.md](03-protocol.md) 第 4 节。
跳频采用逐包跳频，见 [03-protocol.md](03-protocol.md) 第 5 节；
法规约束同节 5.2。

两条链路默认都用 FLRC。改调制类型（FLRC ↔ LoRa）在射频侧比单纯改频率贵得多，
共用调制能让每次切换只改频率。LoRa 只作远距兜底档。

## 4. 时序预算（背景数据）

取自 LR2021 datasheet Rev 1.1 Table 3-23（typical 值）。这些数是射频栈内部的
时序，用来估算时隙余量。

| 符号 | 含义 | typ |
| --- | --- | --- |
| `PLL_LOCK` | 合成器锁定 | 32 µs |
| `TSFSTX1` | FS → Tx | 45 µs |
| `TSFSRX2` | FS → Rx（FSK／FLRC） | 37 µs |
| `TSTXRX` | Tx → Rx | 91 µs |
| `TSRXTX` | Rx → Tx | 107 µs |
| `TSXORX` | STDBY_XOSC → Rx | 84 µs |
| `TSXOTX` | STDBY_XOSC → Tx | 76 µs |

一次「sub-GHz 发 → 2.4G 收」的切换约 `Tx→FS(54) + PLL(32) + FS→Rx(37) ≈ 123 µs`，
来回约 250 µs。按 150 Hz 控制帧率（周期 6.67 ms）算，切换开销约 3.7%。

收发路径按频率自动分流：`ral_lr20xx_bsp_get_rx_cfg()` 在 `freq >= 1.6 GHz`
时选 HF 通道，否则 LF——所以我们只管填频率。

### 速率档位

| 档位 | FLRC 原始速率 | 有效速率（CR 3/4） | 用途 |
| --- | --- | --- | --- |
| 近距高速 | 2.6 Mbps | 1.95 Mbps | 2.4G 数据链路默认 |
| 均衡 | 1.3 Mbps | 0.975 Mbps | 控制链路默认 |
| 远距 | 0.65 Mbps | 0.488 Mbps | 控制链路降级 |
| 兜底 | 0.325 Mbps | 0.244 Mbps | 控制链路保底 |

粗算：6.67 ms 周期里分 40% 给 2.4G 数据链路 ≈ 650 字节/周期 ≈ **97 kB/s**。
完整速率与灵敏度表见 [05-lr2021.md](05-lr2021.md)。

## 5. 协议设计

帧格式、包类型、链路状态机与分时结构见 [03-protocol.md](03-protocol.md)。
身份与密钥见 [02-security.md](02-security.md)。

透传数据的两种语义，做成可切换 profile：

- **best-effort**：裸字节流，缓冲满即丢，附溢出计数。延迟最低。
- **ARQ**：长度前缀 + 序列号 + ACK/重传。适合 MAVLink 这类不能丢的数据。

QoS 分档按场景切换 profile，而不是散落在代码里。

## 6. 软件分层

```
include/zlrs/proto/   协议核心头文件
lib/proto/            协议核心：帧编解码、分片重组、FHSS 跳频表。
                      纯 C、无 Zephyr 依赖，空口 CRC 由射频硬件负责
lib/link/             链路层：会话与配对、PSA 的 HKDF 与 MAC、
                      把帧翻译成 RAC 事务
app/                  Zephyr 应用：配置（短语 -> PBKDF2）、线程、串口、USB
tests/host/           主机侧单测（不依赖 Zephyr）
docs/                 设计文档
```

分层原则：协议核心保持纯 C、可在主机上编译测试；`lib/link/` 与 `app/`
依赖 Zephyr 与 USP，加密操作通过 PSA 走。

射频侧的一切——SPI 时序、校准、频段选择、收发调度——都在 `usp_zephyr` 里，
本仓库不含射频驱动。

## 7. 初始化顺序

构建环境见 [README](../README.md)：`tools/setup-zephyr.sh` 用本仓库的
`west.yml` 建工作区，`tools/zephyr-env.sh` 进环境并指向 Zephyr SDK。

运行时的初始化顺序：

1. `main()` 里先 `SMTC_SW_PLATFORM_INIT()` 等待 USP/RAC 线程就绪
2. `smtc_rac_open_radio(priority)` 分别为控制链路与数据链路开 access id
3. 用 `smtc_rac_get_context()` 拿上下文，填 `modulation_type`、
   `radio_params.flrc.*`、收发缓冲、`scheduler_config` 回调
4. 启动串口线程，进入链路主循环

`smtc_rac_init()` 由 USP 线程调用，应用不重复初始化。

应用侧注册 usp 的日志模块（`LOG_MODULE_REGISTER(usp, ...)`），见
`app/src/usp_log.c`。

## 8. 诊断与可观测性

诊断**不另建通道**：链路质量走 CRSF 既有的链路统计帧，见
[03-protocol.md](03-protocol.md) 2.7。数据链路的溢出与丢弃字节数没有现成的
CRSF 字段承载，不上报。
