# 载板

目标形态：把 RP2350-Zero 与 Core2021 的连线固定在一块 PCB 上，对外给 USB-C、按键与两路 6P 串口，由 5V 供电。两层板，没有射频布局约束（模块自带匹配网络与天线座）。

原型（两模块直接连线）见 [04-prototype](04-prototype.md)；引脚与别名的定义见[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md)。

## 1. 端口

| 口 | 型号 | 内容 |
| --- | --- | --- |
| J1（CRSF） | JST-GH 1.25 mm，6P，卧式 | Pixhawk Telemetry Port 线序：`5V_IN` + CRSF |
| J2（数据） | JST-GH 1.25 mm，6P，卧式 | 同一套线序：`5V_IN` + 数据口 UART1 |
| USB-C | RP2350-Zero 模块自带 | 数据、配置、烧写；不引到 PCB |
| SW1 按键 | SMD 轻触 3×4 mm | GP12 ↔ GND：长按配对 |
| 状态灯 | 模块自带 WS2812（GP16） | PCB 上不引 |

### 1.1 6P 线序（DS-009 Telemetry Port，外设侧视角）

排线是 1:1 的，**方向交叉发生在外设侧**（飞控的 pin 2 是 TX 输出，本板的 pin 2 就是 RX 输入）；不做硬件流控，CTS / RTS 悬空。

| pin | 飞控侧 | 本板 | 接到 |
| --- | --- | --- | --- |
| 1（红线） | VCC +5V | `5V_IN` | 模块 5V 脚 |
| 2 | TX（飞控输出） | RX | UART0_RX = GP1（J2 为 GP9） |
| 3 | RX（飞控输入） | TX | UART0_TX = GP0（J2 为 GP8） |
| 4 | CTS | 悬空 | 不做流控 |
| 5 | RTS | 悬空 | 不做流控 |
| 6 | GND | GND | GND |

两个口的 `5V_IN` 各自经一颗肖特基（SS14 / BAT54）后并到 5V 轨：从任一口取电，两口同时接电源也不反灌。

## 2. 网络

模块之间：

| 信号 | Zero | Core2021 |
| --- | --- | --- |
| SPI0 SCK / MOSI / MISO / CS | GP6 / GP7 / GP4 / GP5 | 对应脚 |
| BUSY / NRESET / DIO8 | GP20 / GP21 / GP22 | 对应脚 |
| 电源 | 3V3、GND | 3V3、GND（3V3 由 Zero 供） |

对外：

| 网络 | 连接 |
| --- | --- |
| `5V_IN` | J1 / J2 第 1 针 → 各自的二极管 → 输入保护 → Zero 的 5V 脚 |
| `CRSF_TX` / `CRSF_RX` | J1 第 3 / 第 2 针 ↔ GP0 / GP1 |
| `DATA_TX` / `DATA_RX` | J2 第 3 / 第 2 针 ↔ GP8 / GP9 |
| `GND` | J1、J2、两个模块、铺铜 |

## 3. 电源

- `5V_IN` → 自恢复保险 + TVS → 100 µF + 10 µF + 0.1 µF → Zero 的 5V 脚
- Core2021 侧就近加 10 µF + 0.1 µF
- GND 两面整块铺铜，模块焊盘附近多打过孔

## 4. 机械与装配

- **两面贴装**：Top = RP2350-Zero（元件面朝上），Bottom = Core2021（元件面朝下）
- USB-C 那一侧让模块探出载板 2–3 mm，方便插线；天线座在相反的短边，该方向留 15–20 mm 净空（不铺铜、不走线、不放器件）
- 安装孔 2 个，Ø2.2 mm（M2），对角放置，不落在模块投影范围内
- 按键放 Bottom 面（Top 面基本被 Zero 占满）
- SPI 走线尽量短（< 20 mm），不跨天线净空区；5V / 3V3 走线 ≥ 0.5 mm
- 邮票孔焊盘按模块图纸（外延 0.3 mm、四周留 0.2 mm 装配余量）
- J1 / J2 用卧式，放同一长边、离开天线净空区，丝印标 `5V/GND` 与 `TX/RX`
