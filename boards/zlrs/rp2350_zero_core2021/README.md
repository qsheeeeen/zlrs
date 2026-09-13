# Waveshare RP2350-Zero + Core2021

原型硬件：微雪 RP2350-Zero（RP2350A，4 MB flash，Cortex-M33）接一颗
微雪 Core2021（Semtech LR2021）模块。板级定义把射频接线、数据串口、按键和
flash 分区都固定在 devicetree 里，应用只认 `zlrs-*` 别名，不写引脚号。

板子叫 `rp2350_zero_core2021` 而不是 `rp2350_zero`：Zephyr 4.4 树内已经有
`waveshare/rp2350_zero`（裸板，没有射频），板名必须全局唯一；这里的差异是多了
一颗 Core2021 与一整套接线，所以 vendor 用自己的 `zlrs`。

## 接线

| 信号 | RP2350-Zero | 说明 |
| --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | 射频总线，16 MHz |
| SPI0 CS | GP5 | 普通 GPIO 片选（`cs-gpios`） |
| BUSY | GP20 | 模块忙，高有效 |
| NRESET | GP21 | 复位，低有效 |
| DIO8 | GP22 | IRQ |
| UART1 TX / RX | GP8 / GP9 | `zlrs-data-uart`，透传口 |
| 按键 | GP12 | 对地，`zlrs-button`，内部上拉 |
| UART0 TX / RX | GP0 / GP1 | 控制台 |
| WS2812B（状态灯） | GP16 | 板上自带，走 PIO0，别名 `zlrs-led`，语义见 [doc/06-ui.md](../../../doc/06-ui.md) |

射频总线没用 Pico 默认的 GP16-19：GP16 在 RP2350-Zero 上是板载 WS2812B 的
数据脚，留给它当状态灯（PIO 驱动，接法抄上游 waveshare 板）。片选走 `cs-gpios`
而不是硬件 CSN，与 Semtech 自家 shield 的做法一致。

按键除了 `zlrs-button`，还挂了 USP 官方示例要的 `smtc-user-button` 别名，
所以 `usp_zephyr/samples/usp/**` 可以直接在这个板上编。

## 两个 SKU → 两个变体

Core2021 的 sub-G 是**两个不同的 SKU**，PA 网络与频段都不同，所以板级定义分成
两个变体（构建目标不同，代码同一份）：

| 变体 | sub-G 硬件 | 支持地区（`CONFIG_ZLRS_APP_REGION`） | 默认 |
| --- | --- | --- | --- |
| `.../rp2350a/m33/hf` | HF：850–930 MHz | FCC915(0) / EU868(1) | FCC915 |
| `.../rp2350a/m33/lf` | LF：410–510 MHz | CN470(2) / CN433(3) | CN470 |

不带变体的 `.../rp2350a/m33` 等于 HF，保留只是方便手滑与旧脚本。

```shell
west build -b rp2350_zero_core2021/rp2350a/m33/hf app   # 868 / 915
west build -b rp2350_zero_core2021/rp2350a/m33/lf app   # 470 / 433
```

板子通过 DT 的 `zlrs,subg-band` 声明自己是哪种 sub-G 硬件；应用启动时校验
"地区 vs 本机硬件"，不匹配就**红灯告警并且不发射频**（法规相关的错误不凑合）。

## Core2021

LR2021 分 HF（2.4 GHz + 850–930 MHz）与 LF（2.4 GHz + 410–510 MHz）两个 SKU，
两个变体各自带自己的 sub-G 标定表：

- HF：`tx-power-cfg-lf` 与 `tx-dbm-to-ua-reg-mode-dcdc-lf-vreg` 用 902 MHz 的
  参考表（Semtech Wio-LR2021 实测），`calibration-freqs` = 897.5 MHz + 2.44 GHz
- LF：用 470 MHz 的参考表，`calibration-freqs` = 433.3 / 470.3 / 498.8 MHz + 2.44 GHz
  （LF 变体要同时覆盖 470 与 433，所以标定点多一个）

两张表都是参考值，上板后要按 Core2021 的 PA 网络重标——验收标准见
[doc/05-roadmap.md](../../../doc/05-roadmap.md) 的 M0。

模块用晶振（不是 TCXO），所以 `tcxo-*` 保持关断；`reg-mode` 用 DC/DC。

## 构建

```shell
west build -b rp2350_zero_core2021/rp2350a/m33/hf app   # 或 .../lf
```

烧写走 BOOTSEL + UF2（`west flash --runner uf2`）或 SWD（openocd / probe-rs / jlink）。

只提供 Cortex-M33 这一档。`west boards` 里还会带出同一个 SoC 的
`rp2350a/hazard3` qualifier（qualifier 由 SoC 定义决定，板子改不了），但本板没有
对应的 dts，用它会掉进 `stub.dts` 然后 Kconfig 报错——原型不跑 RISC-V 那两个核。
