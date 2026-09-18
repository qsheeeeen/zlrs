# 与 ExpressLRS 的关系

本文是 zlrs 与 ExpressLRS（ELRS）关系的唯一出处：参考了什么、对立在哪。其它文档不重复对比，需要时引用本文。

ELRS 侧的数字全部来自其官方文档与源码，链接见文末；对比的是 v4.1。

## 参考

- **性能目标**：控制帧率、灵敏度、数传吞吐这些指标直接对标 ELRS 的公开数字（见 [01-product](01-product.md) 第 3 节）。
- **可靠传输**：数传默认走确认 + 重传（累计 ACK，窗口 4 帧），思路同 ELRS 的 AirPort/MAVLink；差别是我们用独立的确认帧而不是包头 1 bit。见 [09-protocol](09-protocol.md) 4.3。
- **跳频与法规**：逐包跳频、同步信道、按地区的功率与占空比约束，与 ELRS 是同一路做法；差异是我们把地区表做进运行期配置并加了 SKU 一致性校验。
- **周期同步**：连接态稀疏（ELRS 是 5 s）、搜索态密集，这个节奏照抄 ELRS。

## 对立

- **协议不兼容**：不与官方 ELRS 逐字节兼容，不共用绑定与跳频参数，也不支持 ELRS 的硬件模块。
- **不做生态**：没有 Lua 参数协议、Model Match、VTX 控制、Backpack、BLE 手柄这类手台/飞控生态集成。
- **定位不同**：ELRS 是遥控链路生态（几百个硬件目标、WiFi 配置与 OTA、社区实测数据）；zlrs 是一条"控制 + 数传"的自有链路，重点在逐包认证、数传吞吐与法规合规。
- **可验证性做法不同**：ELRS 靠成熟的 CI 与社区实测；我们靠纯 C 分层 + 主机双节点仿真 + 脚本自检。

## 逐项对比

| 维度 | ExpressLRS v4.1 | zlrs |
| --- | --- | --- |
| 定位与形态 | 遥控链路生态：TX 模块、接收机、SPI 接收机、Backpack，几百个 target | 一条链路：RP2350-Zero + Core2021 板，hf/lf 两个 SKU 变体 |
| 频段与调制 | 900M：LoRa 25–250 Hz，另有 K1000；2.4G：LoRa、FLRC `F500`/`F1000`、DVDA；双频模块可跨频段 | 控制链路可 FLRC（2.6/1.3/0.65/0.325 Mbps）或 LoRa（SF5–12），数传固定 2.4G FLRC；单颗射频分时，调制按链路各自选 |
| 控制帧率 | 25–1000 Hz | 150 / 500 Hz 已实现，1000 Hz 是目标（纯 2.4G 形态） |
| 控制协议 | CRSF 输入/输出，16 通道；手台侧 Lua 参数 | CRSF 输入/输出，16 通道；无 Lua |
| 接收机输出 | CRSF / SBUS / SUMD / DJI RS2 / HoTT / PWM / SPI | 只有 CRSF（UART 或 USB CDC） |
| 数传 | AirPort（空口透明串口）；MAVLink 透传（几百 bps～几 kbps） | 专用 2.4G 链路，~97 kB/s 量级；默认可靠重传（ARQ），可选 best-effort |
| 遥测 | LINK 包 512 ms 周期 + DATA 包；MSP/MAVLink/GPS/VTX | 8 B TLM + CRSF 链路统计帧，字段按 CRSF 要求填 |
| 安全 | 绑定短语官方明确"不是密码、不是密钥，只用于防撞"；无逐包认证 | 根密钥 → HKDF 派生；每包 HMAC-SHA256 截断 4 B（分方向，防反射）；配对与会话双方出 nonce；序号防重放；另有 P0 极致性能档（无 tag） |
| 配对/防串 | Binding Phrase 或传统按键；Model Match；Loan Model | 配置工具写根密钥 + 长按按键三次握手；一台设备一个对端 |
| 动态功率 | 有（依赖遥测） | 有（默认开）：按遥测 RSSI/LQ 每秒评估，3 dB 步长 |
| 配置 | Lua、WebUI、Configurator（编译期 Regulatory Domain） | 专用配置 CDC + 运行期配置协议（地区、功率上限等都在里面）；无 WiFi/Lua |
| 升级 | WiFi OTA、Betaflight passthrough、DFU、串口 | UF2（BOOTSEL）+ SWD；带 MCUboot 的变体支持串口恢复 DFU（签名 + 回滚），无无线 OTA |
| 法规 | 编译期预设 | 运行期四张地区表 + SKU/band 一致性校验 + 占空比记账 + 速率/帧率压档 |
| 外设集成 | Serial VTX、Team Racing、Trainer、Sentinel、Backpack、BLE | 无 |
| 双频用法 | Gemini 双发分集；Xrossband 双频冗余 | 两个频段分工（sub-G 控制 + 2.4G 数传），不冗余 |
| 固件与角色 | TX 与 RX 是不同 target | 同一份固件，角色一个 Kconfig |

## 差距（按优先级）

1. **1000 Hz**：ELRS 的 `F1000` 已量产；我们还在 500 Hz，1000 Hz 需要控制链路自己拥有射频（IRQ 驱动逐帧收发）。
2. **手台侧生态**：Lua、Model Match、VTX 控制、头追/教练/团队赛——都不做，靠 CRSF 与飞控/手台互通。
3. **LoRa 档验证**：软件已实现，灵敏度与误包率曲线尚未实测。
4. **无线 OTA**：板子没有 WiFi，要做就得走 2.4G 数传链路。
5. **硬件覆盖**：只有一块板；轻量版还没有实物。

## 我们独有的

- **逐包认证**：ELRS 官方写明绑定短语不是密钥；我们每包都有 HMAC 标签、会话密钥与防重放。
- **数传是主力**：专用 2.4G 链路 + 默认可靠重传，量级上比 AirPort/MAVLink 高两三个数量级。
- **法规进软件**：SKU 校验、占空比记账、帧率与速率压档都在运行期生效。
- **运行期配置**：地区、功率上限这类参数不用重编固件。

## 数据来源

- <https://www.expresslrs.org/info/signal-health/>
- <https://www.expresslrs.org/quick-start/binding/>
- <https://www.expresslrs.org/software/airport/>
- <https://www.expresslrs.org/software/mavlink/>
- <https://www.expresslrs.org/info/telem-bandwidth/>
- <https://www.expresslrs.org/software/dynamic-transmit-power/>
- <https://www.expresslrs.org/quick-start/firmware-options/>
- ELRS 源码（同步间隔、`stubbornAck`、`OTA_Sync_s`）：<https://github.com/ExpressLRS/ExpressLRS>
