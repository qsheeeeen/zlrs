# 载板 v1：RP2350-Zero + Core2021 互连载板

状态：**草稿**（等两块模块的实物尺寸与邮票孔坐标核对后定稿）。

定位：只解决连线与接插口——把两颗模块的连线固定在一块 PCB 上，对外给 J1/J2 两个接插件；不引 RF、不自带 USB、不加端口、不加器件（只有去耦、输入保护、二极管或与一颗按键）。端口按 Pixhawk 规范引到 J1/J2（见第 6 节），两层板足够，没有射频布局约束。

原型（两模块直接连线、只靠 USB）见 [05-prototype.md](05-prototype.md)；本文是它的下一步硬件形态。

## 0. 符合 Pixhawk 接口规范

对齐的是 **DS-009 Pixhawk Connector Standard**（连接器与串口线序）：

- 连接器用 **JST-GH 1.25 mm**
- 任何串口都用 **Telemetry Port** 那套 6 针线序（见下表）
- 原文要点：*"Cables should be 1:1 to the peripheral and TX/RX as well as CTS/RTS should be crossed on the peripheral side"* —— 排线是 1:1 的，**方向交叉发生在外设侧**（飞控的 pin 2 是 TX 输出，外设的 pin 2 就是 RX 输入）
- 不支持硬件流控的端口把 CTS / RTS 悬空

**DS-017 Pixhawk Radio Interface 不适用**：那是 100 W 级电台的 40-pin 板对板接口（10–20 V、以太网/USB），与本板不是一类东西。

## 1. 端口

| 口 | 型号 | 内容 |
| --- | --- | --- |
| J1（CRSF） | JST-GH **1.25 mm**，6P，卧式 | Pixhawk Telemetry Port 线序，见 1.1；`5V_IN` + CRSF |
| J2（数据） | JST-GH **1.25 mm**，6P，卧式 | 同一套线序；`5V_IN` + 数据口 UART1 |
| USB-C | RP2350-Zero 模块自带 | 控制台（USB CDC）+ BOOTSEL 烧写；USB 不引到 PCB |
| SW1 按键 | SMD 轻触 3×4 mm | GP12 ↔ GND：长按配对、开机按住进配置模式 |
| 状态灯 | 模块自带 WS2812（GP16） | PCB 上不引 |

没有 SWD、控制台也不单独引口——它在模块的 USB-C 上（USB CDC），插上电脑就能看日志与烧写。

### 1.1 两个 6P 口的线序（DS-009 Telemetry Port，外设侧视角）

| pin | 飞控侧（DS-009） | 本板（外设侧） | 接到 |
| --- | --- | --- | --- |
| 1（红线） | VCC +5V | `5V_IN` | 模块 5V 脚 |
| 2 | TX（飞控输出） | **RX** | UART0_RX = GP1 |
| 3 | RX（飞控输入） | **TX** | UART0_TX = GP0 |
| 4 | CTS（输入） | 悬空 | 本板不做流控 |
| 5 | RTS（输出） | 悬空 | 本板不做流控 |
| 6 | GND | GND | GND |

J2 用同一套线序，第 2/3 针接数据口：**RX = GP9、TX = GP8**；第 4/5 针同样悬空。

两个口的 `5V_IN` 各自经一颗肖特基（SS14 / BAT54）**二极管或**后并到 5V 轨：既能从任一口取电，又不会在两个口同时接电源时互相反灌（压降 0.3–0.5 V，模块 5V 脚允许 1.8–5.5 V）。

## 2. 网络清单

模块之间（RP2350-Zero ↔ Core2021）：

| 信号 | Zero | Core2021 |
| --- | --- | --- |
| SPI0 SCK / MOSI / MISO / CS | GP6 / GP7 / GP4 / GP5 | 对应脚 |
| BUSY | GP20 | BUSY |
| NRESET | GP21 | NRESET |
| DIO8（IRQ） | GP22 | DIO8 |
| 电源 | 3V3、GND | 3V3、GND（3V3 由 Zero 供） |

对外与电源：

| 网络 | 连接 |
| --- | --- |
| `5V_IN` | J1 / J2 的第 1 针 → 各自的二极管 → 输入保护 → RP2350-Zero 的 5V 脚 |
| `CRSF_TX` / `CRSF_RX` | J1 第 3 / 第 2 针 ↔ GP0 / GP1（UART0） |
| `DATA_TX` / `DATA_RX` | J2 第 3 / 第 2 针 ↔ GP8 / GP9（UART1） |
| `GND` | J1、J2、两个模块、铺铜 |

## 3. 电源

- `5V_IN` → 自恢复保险 + TVS → 100 µF + 10 µF + 0.1 µF → Zero 的 5V 脚
- Core2021 侧就近加 10 µF + 0.1 µF
- GND 两面整块铺铜，模块焊盘附近多打过孔
- 独立 LDO 与电压检测**不做**

## 4. 机械

- 外形 **≈22 × 28 mm**、板厚 1.6 mm、外框 R1（最终按实物微调）
- **两面贴装**：Top = RP2350-Zero（元件面朝上），Bottom = Core2021（元件面朝下）
- **USB-C 那一侧让模块探出载板 2–3 mm**，方便插线；**天线座在相反的短边**，该方向留 15–20 mm 净空（不铺铜、不走线、不放器件）
- 安装孔 **2 个**，Ø2.2 mm（M2），对角放置，且不落在任一模组的投影范围内
- 按键放 **Bottom** 面：Top 面 18 × 23.5 基本被 Zero 占满，3×4 mm 按键没位置

## 5. 布局与装配

- SPI 走线尽量短（< 20 mm），不跨天线净空区
- 5V / 3V3 走线 ≥ 0.5 mm；按键走线随意
- 邮票孔焊盘按模块图纸（外延 0.3 mm、四周留 0.2 mm 装配余量），钢网按贴装焊盘开
- 贴装顺序：先贴 Bottom 的 Core2021，再贴 Top 的 RP2350-Zero；两块模块元件面都朝外
- J1 / J2 用卧式（side-entry），放同一长边、离开天线净空区，丝印标 `5V/GND` 与 `TX/RX`

## 6. 固件

CRSF = UART0 / GP0-GP1 → J1，数据 = UART1 / GP8-GP9 → J2，控制台在 USB 第三个 CDC；主机把某块 CDC 打开时该端口走 USB（见 [01](01-system.md) 2.1）。USB-C 同时是 BOOTSEL 烧写口。hf / lf 两个 SKU 变体按手上的 Core2021 选。

## 7. 待确认

- 两块模块的实际外形尺寸、邮票孔坐标与天线座位置（按实物量）
- PCB 最终轮廓尺寸与两个安装孔的位置
- J2 第 1 针：接 5V（二极管或）还是留空（现按"二极管或"写的，两颗肖特基）
