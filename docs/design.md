# zlrs 设计文档

基于 Zephyr 自研的低延迟遥控 + 串口透传链路，射频芯片为 Semtech LR2021。

## 1. 目标与边界

- 主诉求是 **UART 串口透传**（把一端的串口字节透明送到另一端）。
- 不要求与官方 ExpressLRS 逐字节兼容；ELRS 仅作为协议结构上的参考。
- 硬件为 **单颗 LR2021 分时双频**：sub-GHz 跑控制链路，2.4GHz 跑透传数据链路。
- 射频栈**不自己写**，用 Semtech 官方
  [usp_zephyr](https://github.com/Lora-net/usp_zephyr) 的 RAC 层。

方案能成立的前提来自 datasheet：LR2021 是半双工、单 switchless 前端，同一时刻
只能在一个频段工作，但频段切换开销很小（见第 4 节）。

## 2. 硬件与软件基线

| 项目 | 选择 |
| --- | --- |
| 射频 | Semtech LR2021（QFN32），SPI + BUSY + DIO |
| 原型硬件 | XIAO nRF54L15 + Wio-LR2021（Semtech 官方验证组合） |
| 射频栈 | `usp_zephyr` v1.0.0 + `usp` v1.0.0，使用 RAC/RALF 层 |
| Zephyr | v4.2.0（与 usp_zephyr 对齐） |
| 仓库拓扑 | T2，zlrs 自己当 manifest 仓库并存放应用 |

不用 LBM（LoRa Basics Modem）——那是 LoRaWAN 那套；我们要的是裸包收发，
即 RAC（Radio Access）层。相关能力在 `smtc_rac_flrc.c` / `smtc_rac_fsk.c` /
`smtc_rac_lora.c`，示例见 `samples/usp/sdk/packet_error_rate_flrc`。

## 3. 系统架构

两个逻辑链路共用一个射频：

| 链路 | 频段 | 调制 | 承载 | RAC 优先级 | 优化目标 |
| --- | --- | --- | --- | --- | --- |
| 控制链路 | sub-GHz | FLRC（LoRa 兜底） | RC 通道 + 低速遥测 | `HIGH` | 距离与鲁棒性 |
| 数据链路 | 2.4GHz | FLRC | 串口透传分段流 | `MEDIUM` | 吞吐与延迟 |

关键设计选择：**两条链路默认都用 FLRC**。改调制类型（FLRC ↔ LoRa）在射频侧比
单纯改频率贵得多，共用调制能让每次切换只改频率。LoRa 只作远距兜底档。

控制链路是权威状态来源：它在线即视为系统在线；数据链路掉线只降吞吐，不影响操控。

分时不靠自己写调度器，而是把两条链路注册成两个 RAC 客户端，用优先级让
radio planner 仲裁——详见 [threading.md](threading.md)。

## 4. 时序预算（背景数据）

取自 LR2021 datasheet Rev 1.1 Table 3-23（typical 值）。射频栈由 USP 接管后，
这些数不再是我们直接下发的参数，但用来估算时隙余量仍然有效。

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
数量级估计，需要真板标定。完整速率与灵敏度表见 [lr2021.md](lr2021.md)。

## 5. 协议设计

帧格式、包类型、链路状态机与分时结构见 [protocol.md](protocol.md)。
身份与密钥见 [identity.md](identity.md)。

透传数据的两种语义，做成可切换 profile：

- **best-effort**：裸字节流，缓冲满即丢，附溢出计数。延迟最低。
- **ARQ**：长度前缀 + 序列号 + ACK/重传。适合 MAVLink 这类不能丢的数据。

QoS 分档按场景切换 profile，而不是散落在代码里。

## 6. 软件分层

```
include/zlrs/proto/   协议核心头文件
lib/proto/            纯 C 协议核心：crc / fhss / uid / (待补) frame / serial / link
lib/link/             (待建) 链路层：把帧翻译成 RAC 事务，管理链路状态机
app/                  Zephyr 应用：线程、串口、USB
tests/host/           主机侧单测（不依赖 Zephyr）
docs/                 设计文档
```

分层原则不变：协议核心保持纯 C、可在主机上编译测试；只有 `lib/link/` 和
`app/` 依赖 Zephyr 与 USP。

射频侧的一切——SPI 时序、校准、频段选择、收发调度——都交给 `usp_zephyr`，
我们不再维护自己的 LR2021 驱动。

## 7. 初始化顺序

1. `west init -l zlrs && west update`（拉 zephyr v4.2.0 + usp_zephyr + usp）
2. `main()` 里先 `SMTC_SW_PLATFORM_INIT()` 等待 USP/RAC 线程就绪
3. `smtc_rac_open_radio(priority)` 分别为控制链路与数据链路开 access id
4. 用 `smtc_rac_get_context()` 拿上下文，填 `modulation_type`、
   `radio_params.flrc.*`、收发缓冲、`scheduler_config` 回调
5. 启动串口线程，进入链路主循环

不要自己调 `smtc_rac_init()`——USP 线程负责。

## 8. 风险与验证顺序

风险按优先级：

1. **协作式线程模型下的射频响应延迟**：串口满载时射频回调会被推迟多久。
   见 [threading.md](threading.md) 第 5 节。
2. **RAC 优先级抢占的真实行为**：控制链路提交时，正在跑的数据链路事务是被
   打断还是排队，直接决定透传的延迟特性。
3. **`SMTC_RAC_SCHEDULED_TRANSACTION` 的时隙精度**：示例里 delay 是 ms 量级，
   能否支撑想要的时隙粒度待验证。
4. **FLRC 带宽参数在 datasheet 内部不一致**（Table 18-1 与
   `SetFlrcModulationParams` 小节列出的 DSB 带宽不同），标定时留意。

验证顺序：

1. 官方 `packet_error_rate_flrc` 示例在 XIAO nRF54L15 + Wio-LR2021 上跑通，
   确认工具链与硬件没问题。
2. 用官方示例量第 1、3 项风险。
3. 主机侧把协议核心与帧格式定稿并测试。
4. 接上 `lib/link/`，先只跑控制链路，再加透传。

## 9. 待定项

- 身份层尚未落地的决策见 [identity.md](identity.md) 第 8 节
  （密钥输入形式、tag 长度、加密默认值、KDF/MAC 选型、KDF 放在哪一侧）。
- 控制链路的帧率与 RC 通道数。
- 透传的目标吞吐（暂按 ~100 kB/s 量级设计）。
- 透传是否要求不丢（决定默认 profile）。
- 最终 MCU 与自研板（当前先用官方 EVK 组合）。
- 频段与信道规划（sub-GHz 受占空比/跳频约束，2.4G 宽松）。
- 法规约束反推到协议的具体参数（最少跳频信道数、单信道驻留时间、占空比上限）。
