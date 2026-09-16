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

库层的开关（`lib/*/Kconfig`）由应用的功能开关反选：`ZLRS_APP_CONFIG_MODE` / `ZLRS_APP_STATUS_LED` / `ZLRS_APP_CRSF_PORT` 一关，对应的 `ZLRS_CONFIG` / `ZLRS_LED` / `ZLRS_CRSF` 就可以一起裁掉（应用里留了退化实现，编得过）。协议核心、链路层、地区配置层与 CRSF 编解码是应用永远需要的，由 `ZLRS_APP_CORE` 固定选中。

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

### 2.4 启动链与固件升级

板级有两个目标家族：`.../m33/hf`、`.../m33/lf`（应用直接放在 flash 里）与
`.../m33/hf/mcuboot`、`.../m33/lf/mcuboot`（多一个 bootloader 与两槽固件分区）。
两者共用同一份应用代码，差别只在分区表与是否带 bootloader；具体分区数字见
[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md#分区与启动)。

| | 不带 bootloader | 带 MCUboot |
| --- | --- | --- |
| 启动链 | RP2350 bootrom → 应用 | RP2350 bootrom → MCUboot（校验签名）→ slot0 里的应用 |
| flash 布局 | code 1 MB + storage 3 MB | mcuboot 128 KB + slot0/slot1 各 832 KB + storage 2.25 MB |
| 升级 | 按住 BOOTSEL 拖 `zephyr.uf2`（或 SWD） | 拖一次 `zephyr.signed.uf2` 起步；之后走串口恢复，不用按键 |
| 回滚 | 无 | `swap using offset`：升级失败可以退回上一版 |

**为什么上 MCUboot。** 三件事：① 现场升级不需要拆机、不需要调试器；② 镜像签名
（ECDSA-P256）+ 启动校验，坏了能回滚；③ 砖机也能救——bootloader 起来后没有可启动的
镜像就停在串口恢复里。

**升级通道：MCUboot 串口恢复。** bootloader 自己收 mcumgr 帧，走数据口的 UART1
（GP8/GP9，devicetree chosen `zephyr,uart-mcumgr`），写进 slot1 后由 MCUboot 换槽。
bootloader 里不带 USB 栈是有意的：少一份复杂度，恢复时用一根 USB 串口线就行。
（USB CDC 恢复是后面可选的加法，128 KB 的 bootloader 分区就是给它留的余量。）

进恢复模式有四条入口，都不碰 BOOTSEL：

| 入口 | 什么时候生效 | 说明 |
| --- | --- | --- |
| 应用侧触发 | 设备正常运行中 | 数据口收到 16 字节魔数 `zlrs!bootloader!`（`scripts/zlrs-recovery.py`）→ 应用置 boot mode 并重启 → bootloader 读到标志就停在恢复模式。**升级一台健康设备不用碰设备本身** |
| 启动窗口 | 每次复位后 300 ms | 窗口里收到**合法的 mcumgr 帧**就停下来升级——主机脚本先开始发 `image list`，再让设备复位（拔插 USB 或按复位键） |
| 没有可启动镜像 | 砖机 | 校验失败/镜像为空时停在恢复模式，永远不会"转圈" |
| 按键 | 不用 | bootloader 不占板上按键：它在应用里有"开机按住进配置"的语义，抢占会让两边都变歧义 |

boot mode 那块标志放在 SRAM 起始处的 4 字节里（Zephyr 树里 RP2350 专用的
`rp2350-boot-mode-retention.dtsi`，`sram0` 相应让开 4 字节），由 `RETENTION_BOOT_MODE`
读写，应用与 bootloader 看到的是同一份 DT。魔数匹配是逐字节的：只有当前缀的那几个
字节会被扣住，一旦不匹配就原样交回透传队列，所以正常数据既不丢也不重复。这个入口
在 Kconfig 里是 `CONFIG_ZLRS_APP_UPDATE_TRIGGER`，量产/竞速构建可以关掉。

`CONFIG_BOOT_SERIAL_PIN_RESET`（"复位由引脚引起就进恢复"）**故意没开**：RP2350 的
hwinfo 把 POWMAN 的 `HAD_RUN` 位映射成 `RESET_PIN`，这一位在"芯片跑过"这件事上的
语义比"按了复位键"宽，没上板确认之前开它就有"每次上电都停在恢复模式"的风险。

主机侧：`scripts/zlrs-recovery.py --port <数据口>` 负责触发（`--self-test` 不用设备），
镜像上传用 mcumgr 客户端（Go 版 `mcumgr` 或 Python 的 `smpmgr`），上传的是
`zephyr.signed.bin`。把"触发 + 上传 + 查回滚状态"合成一条脚本是后面的事
（见 [05-roadmap.md](05-roadmap.md) M1 的升级链路待做项）。

**密钥。** 开发期用 MCUboot 自带的测试密钥（`bootloader/mcuboot/root-ec-p256.pem`），
bootloader 校验与应用签名用的是同一把——由 sysbuild 统一设置，两边不会不一致。
量产前生成自己的密钥（`imgtool genkey -t ecdsa-p256`），私钥不进仓库，并且**要重新烧
一次 bootloader**：公钥在 bootloader 里。

**和 2.3 的调试开关怎么配合。** 带 bootloader 的构建同样支持那批分层开关：第一次上板
建议先烧"最简形态"（P0 + 关跳频 + 关握手 + 关遥测）确认启动链与日志，再逐层打开。
注意 bootloader **不使用板上那颗按键**（按键在应用里已经有"开机按住进配置"的语义），
所以它不会和配置模式抢。

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

两种模式都是 **`CONFIG_ZLRS_APP_BAND_ROLE_*`** 选出来的编译期配置（不是运行时可切换项，配置协议里没有这一项）：

- **远距模式**（默认，`..._SUBG_CONTROL=y`）：控制链路跑本地区域的 sub-G 频段（由 SKU 与地区表决定，见 `lib/region`），数据链路跑 2.4G。控制链路吃法规门（功率/速率上限、占空比）。
- **竞速模式**（`..._SUBG_DATA=y`）：控制链路独占 2.4G（`CONFIG_ZLRS_APP_RACING_CONTROL_RATE_BPS/_HZ`，默认 2.6 Mbps / 500 Hz），数据链路让到 sub-G 并受地区法规约束；不想留数据链路就直接 `CONFIG_ZLRS_APP_LINK_DATA=n`。法规门只作用在 sub-G 那条链路上——2.4G 没有地区表。

超帧周期在两边的算法一样：数据链路的窗口按 `data_every_n` 摊进每一帧，控制帧率被它拖下来多少由计划算出来（例如 2.4G 控制 500 Hz + sub-G 数据 0.325 Mbps 的 128 B 帧 → 420 Hz，见主机侧用例 `region: racing plan`）。

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
