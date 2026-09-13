# Waveshare RP2350-Zero

原型硬件：微雪 RP2350-Zero（RP2350A，4 MB flash，Cortex-M33）接一颗
微雪 Core2021（Semtech LR2021）模块。板级定义把射频接线、数据串口、按键和
flash 分区都固定在 devicetree 里，应用只认 `zlrs-*` 别名，不写引脚号。

## 接线

| 信号 | RP2350-Zero | 说明 |
| --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | 射频总线，16 MHz |
| SPI0 CS | GP5 | 普通 GPIO 片选（`cs-gpios`） |
| BUSY | GP20 | 模块忙，高有效 |
| NRESET | GP21 | 复位，低有效 |
| DIO8 | GP22 | IRQ |
| UART1 TX / RX | GP10 / GP11 | `zlrs-data-uart`，透传口 |
| 按键 | GP12 | 对地，`zlrs-button`，内部上拉 |
| UART0 TX / RX | GP0 / GP1 | 控制台 |
| WS2812B | GP16 | 板上自带，本板定义不使用 |

射频总线没用 Pico 默认的 GP16-19：GP16 在 RP2350-Zero 上是板载 WS2812B 的
数据脚，留给它。片选走 `cs-gpios` 而不是硬件 CSN，与 Semtech 自家 shield 的
做法一致。

## Core2021

LR2021 分 HF（2.4 GHz + 850–930 MHz）与 LF（2.4 GHz + 410–510 MHz）两个 SKU，
本板定义按 **HF（900M）** 配置：`calibration-freqs` 与 `tx-power-cfg-lf` 参考
Wio-LR2021 在 902 MHz 的实测表。换 LF 模块或换 region 时，这两张表要按实际频点
重标——验收标准见 [docs/05-roadmap.md](../../../docs/05-roadmap.md) 的 M0。

模块用晶振（不是 TCXO），所以 `tcxo-*` 保持关断；`reg-mode` 用 DC/DC。

## 构建

```shell
west build -b waveshare/rp2350_zero/rp2350a/m33 app
```

烧写走 BOOTSEL + UF2（`west flash --runner uf2`）或 SWD（openocd / probe-rs / jlink）。
