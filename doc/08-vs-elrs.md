# 与 ExpressLRS 的功能对比

本文只做事实对照与差距判断，不重复本项目的设计（那些在 01–07）。ELRS 侧的数字
全部来自其官方文档，链接见文末；对比的是 ELRS v4.1。

## 一句话

ELRS 是成熟的遥控链路**生态**（几百个硬件目标、WiFi 配置与 OTA、手台集成、
双频冗余、大量实测数据）；zlrs 是"控制 + 透传"两条链路、逐包认证、地区合规的
**原型**。两者重叠的只有"CRSF 遥控 + 低带宽遥测"这一块。

## 逐项对比

| 维度 | ExpressLRS v4.1 | zlrs 现状 |
| --- | --- | --- |
| 定位与形态 | 遥控链路生态：TX 模块、接收机、SPI 接收机、Backpack，几百个 target | 一块原型板（RP2350-Zero + Core2021，hf/lf 两个 SKU 变体）+ 载板规格 |
| 频段与调制 | 900M：LoRa 25–250 Hz，另有 K1000（1000 Hz FSK）；2.4G：LoRa 50–500 Hz、FLRC `F500`/`F1000`、DVDA（FLRC/FSK）；双频模块可跨频段 | 只有 FLRC（2.6/1.3/0.65/0.325 Mbps）；sub-G 控制 + 2.4G 透传，同一颗射频分时；"按链路各自选调制（LoRa）"方案已定，未实现 |
| 控制帧率 | 25–1000 Hz；`F1000` 发送时长 388.8 µs、间隔 1000 µs（约 39% 空口占比） | 150 / 500 两档，且被地区占空比与空中时间压档（EU868 → ~46 Hz，窄带速率 → ~470 Hz）；1000 Hz 未实现——它要求控制链路自己拥有射频（IRQ 驱动 / 硬件 auto-Tx/Rx），不是换个 RAC 调用 |
| 控制协议 | CRSF 输入/输出，16 通道；手台侧 Lua 参数 | CRSF 输入/输出，16 通道；无 Lua/参数协议 |
| 接收机输出 | CRSF / SBUS / SUMD / DJI RS2 / HoTT / PWM，另有 SPI 接收机直插飞控 | 只有 CRSF（UART 或 USB CDC） |
| 透传/数据 | AirPort（固件选项，把一对 TX/RX 变成空口透明串口）；MAVLink 透传（几百 bps～几 kbps）；standalone 遥测电台模式 | **专用 2.4G 链路**：MTU 128 B、粗估 ~97 kB/s；可选 ARQ（累计 ACK + 重传 + 重同步）与背压计数 |
| 遥测 | LINK 包 512 ms 周期 + DATA 包，遥测比率 1:128…1:2，量级几百 bps；MSP/MAVLink/GPS/VTX | 8 B TLM（LQ/RSSI/SNR/速率档/功率档/状态）+ CRSF 链路统计帧；无 MSP/MAVLink 适配层（透传能承载原始字节） |
| 安全 | 绑定短语官方明确"不是密码、不是加密密钥，只用于防撞"；无逐包认证/加密 | 根密钥 → HKDF 派生；**每包 HMAC-SHA256 截断 4 B**（分方向，防反射）；配对 nonce 掺进 `pair_key`；序号递增防重放 |
| 配对/防串 | Binding Phrase（≥8 字符）或传统按键；Model Match；Loan Model | 长按配对（三次握手）+ 配对时下发失控值；一台设备一个对端（无 Model Match） |
| 动态功率 | 有：按 RX 上报的信号升降 TX 功率（依赖遥测开启） | 有（默认开）：按遥测 RSSI/LQ 每秒评估，3 dB 步长、2 s 保持、遥测断回上限；下限可配 |
| 配置 | Lua（手台内）、WebUI（WiFi）、Configurator（含编译期 Regulatory Domain） | 配置协议跑数据口 + 命令行工具（`scripts/zlrs-config.py`，带与固件对拍的 self-test）；无 WiFi/Lua |
| 升级 | WiFi OTA、Betaflight passthrough、DFU、串口 | UF2（模块自带 USB-C）+ SWD（原型板）；无 OTA |
| 法规 | 编译期 Regulatory Domain 预设（功率上限等） | 运行期四张地区表 + **SKU/band 一致性校验** + 占空比记账（超限让出时隙并计数）+ 速率/帧率压档 |
| 外设集成 | Serial VTX、Team Racing、Trainer（头追）、Sentinel、Backpack、BLE 手柄 | 无 |
| 双频用法 | Gemini：单频段双发分集；Xrossband：双频同时发同一份 RC（冗余） | 两个频段**分工**：sub-G 跑控制、2.4G 跑透传（不冗余） |
| 固件与角色 | TX 与 RX 是不同 target | **同一份固件**，角色一个 Kconfig |
| 工程可验证性 | 成熟 CI + CRC 测试 / RX 测试 / 单元测试 + 社区实测数据 | 纯 C 分层 + 主机双节点仿真（969 checks）+ 配置工具 self-test + CI（host 测试 + 两变体构建）；**射频层尚未上板** |

## 我们明显缺的（按优先级）

1. **1000 Hz 档**（ELRS 的 `F1000`）：我们目前上限 500 Hz，且窄带/受限地区还会被
   压档。实现它要把控制链路的射频归属从 RAC 手里拿走（直调驱动 + IRQ 驱动逐帧时序，
   或硬件 auto-Tx/Rx）；RAC 的 FLRC burst **不是**这条路——那是单向批量传输。
2. **手台侧生态**：Lua 参数协议、Model Match（多机防串）、VTX 控制、头追/教练/
   团队赛——全缺，且都要与手台/飞控生态对接。
3. **LoRa 调制**：方案已定（按链路各自选），未实现；窄带地区现在只能用 0.325 Mbps
   的 FLRC。
4. **射频层数据与工具链**：PER 曲线、延迟、灵敏度、认证测试——都还挂在 M0/M1。
5. **硬件覆盖与 OTA**：只有一块板；升级靠 UF2（我们不打算做 WiFi OTA——板子没有
   WiFi）。

## 我们独有的（值得守住）

- **逐包认证**：ELRS 官方写明绑定短语不是密钥；我们的每包都有 HMAC 标签、
  会话密钥与防重放。
- **透传是主力**：专用 2.4G 链路 + ARQ，量级上比 AirPort/MAVLink 遥测高两三个
  数量级，而且不需要换固件选项。
- **法规进软件**：SKU/band 校验、占空比记账、帧率与速率压档；ELRS 靠编译期预设
  与用户自觉。
- **可验证的工程结构**：同一份固件 TX/RX、纯 C 协议核心、主机双节点仿真、CI。
- **失控值随配对下发**：链路自己传，不依赖手台侧配置。

## 结论

ELRS 的领先在**生态与成熟度**（目标覆盖、配置/升级体验、实测数据、社区），
这部分靠多年积累，追不划算；我们的差异化在**认证、透传、合规与可验证性**。
资源有限时应该先把上板数据与 1000 Hz 那条路做出来，而不是去补生态。

## 数据来源（ELRS 官方文档）

- RF 模式、速率、灵敏度与 TX 时长：<https://www.expresslrs.org/info/signal-health/>
- 绑定短语"不是密码/密钥"、Model Match：<https://www.expresslrs.org/quick-start/binding/>
- AirPort（空口透明串口）：<https://www.expresslrs.org/software/airport/>
- MAVLink 与吞吐表：<https://www.expresslrs.org/software/mavlink/>
- 遥测带宽与比率：<https://www.expresslrs.org/info/telem-bandwidth/>
- 动态功率：<https://www.expresslrs.org/software/dynamic-transmit-power/>
- Gemini（双频/双发）：<https://www.expresslrs.org/software/gemini/>
- Model Config Match：<https://www.expresslrs.org/software/model-config-match/>
- 接收机串口协议：<https://www.expresslrs.org/software/serial-protocols/>
- 固件选项（含 Regulatory Domains）：<https://www.expresslrs.org/quick-start/firmware-options/>
- WebUI：<https://www.expresslrs.org/quick-start/webui/>
- WiFi 升级：<https://www.expresslrs.org/software/updating/wifi-updating/>
