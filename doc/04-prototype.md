# 原型

现在的实物是**两块模块用线连起来**：微雪 RP2350-Zero 加微雪 Core2021。对外的接口**只有 USB-C**（供电、数据、配置、烧写）；CRSF 与数据是模块焊盘上的两路 UART，要用就自己引线。按键不焊，用一根杜邦线把 GP12 短接到 GND 模拟。

引脚与别名的权威定义见[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md)，端口与模式的设计见 [08-firmware](08-firmware.md) 第 2、5 节。

## 1. 实物构成

| 部件 | 规格 | 备注 |
| --- | --- | --- |
| 主控 | 微雪 RP2350-Zero：RP2350A（Cortex-M33）、4 MB QSPI flash、原生 USB-C、板载 WS2812B | BOOTSEL 与 RESET 在模块上 |
| 射频 | 微雪 Core2021（Semtech LR2021），LF SKU | 单颗分时双频：sub-G + 2.4G |
| 天线 | 433 MHz，接模块天线座 | 2.4G 那一路上板后按模块图纸核对 |
| 供电 | USB-C | Core2021 的 3V3 由 Zero 供，两块模块共地 |
| 按键 | 不焊按键：杜邦线把 GP12 短到 GND | DT 里已配内部上拉 |
| 状态灯 | 模块自带 WS2812（GP16） | 语义见 [11-ux](11-ux.md) |

## 2. 板内连线（Zero ↔ Core2021）

只在 `CONFIG_ZLRS_APP_RADIO=y` 时使用；`=n` 时整块射频不碰（含 SPI 与三根控制线）。

| 信号 | RP2350-Zero | Core2021 | 说明 |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | 对应脚 | 总线 16 MHz |
| SPI0 CS | GP5 | 对应脚 | 普通 GPIO 片选（`cs-gpios`），不用硬件 CSN |
| BUSY | GP20 | BUSY | 高有效，内部上拉 |
| NRESET | GP21 | NRESET | 低有效 |
| DIO8（IRQ） | GP22 | DIO8 | 高有效，内部下拉 |
| 3V3 / GND | 3V3、GND | 3V3、GND | 由 Zero 供 |

两路 UART（GP0/GP1 = CRSF，GP8/GP9 = 数据口）留在模块焊盘上，原型上悬空；控制台不走引脚，只在 USB 上。

## 3. 接口与模式

USB-C 一个口，按运行模式以不同面孔出现：

| 模式 | USB 上是什么 |
| --- | --- |
| 应用 | CRSF / 数据 / 配置 三块 CDC |
| 串口恢复 | 恢复 + 控制台 两块 CDC |
| ROM BOOTSEL | `RPI-RP2` 大容量盘（拖 UF2） |
| 调试 | 上面的基础上多一块 console CDC（日志 + shell），见 [12-debug](12-debug.md) |

| 接口 | 应用 | 配置 | 串口恢复 | ROM BOOTSEL |
| --- | --- | --- | --- | --- |
| USB-C | 三块 CDC | 配置 CDC 跑配置协议，射频停 | 两块 CDC | `RPI-RP2` |
| UART0 GP0/1 | CRSF 收发 420000 | 不转发（射频停） | 不初始化 | 不跑固件 |
| UART1 GP8/9 | 数传透传 | 收到的字节丢弃并计数 | 不用 | 不跑固件 |
| 按键（杜邦线） | 长按 ≥1 s 配对 | 进入后不看按键 | 不用 | BOOTSEL 是芯片级按钮 |
| 状态灯 | 见 [11-ux](11-ux.md) | 白灯 0.5 Hz | MCUboot 不驱动 | 不驱动 |

配置模式由配置工具在配置 CDC 上发命令进入；细节见 [08-firmware](08-firmware.md) 第 5 节。

## 4. 烧录

| 变体 | 第一次 | 之后 |
| --- | --- | --- |
| 不带 bootloader | 按住 BOOTSEL 拖 `zephyr.uf2`，或 SWD | 同上 |
| 带 MCUboot | 两轮 BOOTSEL：先 bootloader，再 `zephyr.signed.uf2` | 应用侧触发进恢复 + `smpmgr` 上传签名镜像 |

一条命令升级：`python3 scripts/zlrs-update.py --port /dev/ttyACM1`。

## 5. 接线与摆放

- 3V3 由 Zero 供 Core2021，两块模块共地。
- SPI 跳线尽量短；GP16 被板载 WS2812B 占着，所以射频 SPI 用 GP4–GP7。
- 天线座那一侧留 15–20 mm 净空（不贴金属、不过线）。
