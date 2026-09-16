# Waveshare RP2350-Zero + Core2021

硬件：微雪 RP2350-Zero（RP2350A，4 MB flash，Cortex-M33）接一颗微雪 Core2021（Semtech LR2021）模块。板级定义把射频接线、两路串口（CRSF 与数据口）、USB 上的三个 CDC、按键和 flash 分区都固定在 devicetree 里，应用只认 `zlrs-*` 别名，不写引脚号。

板子叫 `rp2350_zero_core2021` 而不是 `rp2350_zero`：Zephyr 4.4 树内已经有 `waveshare/rp2350_zero`（裸板，没有射频），板名必须全局唯一；这里的差异是多了一颗 Core2021 与一整套接线，所以 vendor 用自己的 `zlrs`。

## 接线

| 信号 | RP2350-Zero | 说明 |
| --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | 射频总线，16 MHz |
| SPI0 CS | GP5 | 普通 GPIO 片选（`cs-gpios`） |
| BUSY | GP20 | 模块忙，高有效 |
| NRESET | GP21 | 复位，低有效 |
| DIO8 | GP22 | IRQ |
| UART0 TX / RX | GP0 / GP1 | `zlrs-crsf-uart`，CRSF（420000），接 J1 那种手台/飞控 |
| UART1 TX / RX | GP8 / GP9 | `zlrs-data-uart`，透传/配置口（默认 115200） |
| 按键 | GP12 | 对地，`zlrs-button`，内部上拉 |
| USB-C（原生 USB） | — | 三个 CDC：CRSF、数据口、**控制台**（`cdc_acm_uart0/1/2`，别名 `zlrs-*-cdc`；控制台是 `zephyr,console`） |
| WS2812B（状态灯） | GP16 | 板上自带，走 PIO0，别名 `zlrs-led`，语义见 [doc/06-ui.md](../../../doc/06-ui.md) |

射频总线没用 Pico 默认的 GP16-19：GP16 在 RP2350-Zero 上是板载 WS2812B 的数据脚，留给它当状态灯（PIO 驱动，接法抄上游 waveshare 板）。片选走 `cs-gpios` 而不是硬件 CSN，与 Semtech 自家 shield 的做法一致。

**每条链路的端点二选一**：主机把该链路的 CDC 打开就走 USB，否则走 UART；控制台在 USB 第三个 CDC 上（`zephyr,console` = `cdc_acm_uart2`）。规则见 [doc/01-system.md](../../../doc/01-system.md) 2.1 与 `app/src/ports.c`。

按键除了 `zlrs-button`，还挂了 USP 官方示例要的 `smtc-user-button` 别名， 所以 `usp_zephyr/samples/usp/**` 可以直接在这个板上编。

## 两个 SKU → 两个变体

Core2021 的 sub-G 是**两个不同的 SKU**，PA 网络与频段都不同，所以板级定义分成两个变体（构建目标不同，代码同一份）：

| 变体 | sub-G 硬件 | 支持地区（`CONFIG_ZLRS_APP_REGION`） | 默认 |
| --- | --- | --- | --- |
| `.../rp2350a/m33/hf` | HF：850–930 MHz | FCC915(0) / EU868(1) | FCC915 |
| `.../rp2350a/m33/lf` | LF：410–510 MHz | CN470(2) / CN433(3) | CN470 |
| `.../rp2350a/m33/hf/mcuboot` | 同 hf | 同 hf | FCC915 |
| `.../rp2350a/m33/lf/mcuboot` | 同 lf | 同 lf | CN470 |

带 `/mcuboot` 的两个变体多一个 bootloader 与两槽固件分区（见下面"分区与启动"）；
它们的构建要加 `--sysbuild`，把 MCUboot 一起编出来。

**板子只有这两种形态**，构建时必须显式指定变体：不带变体的 `rp2350_zero_core2021/rp2350a/m33` 没有对应的硬件，板级定义里也没有它的 dts，用它构建会失败（Zephyr 会退回 `stub.dts`）。

```shell
west build -b rp2350_zero_core2021/rp2350a/m33/hf app   # 868 / 915
west build -b rp2350_zero_core2021/rp2350a/m33/lf app   # 470 / 433
```

板子通过 DT 的 `zlrs,subg-band` 声明自己是哪种 sub-G 硬件；应用启动时校验"地区 vs 本机硬件"，不匹配就**红灯告警并且不发射频**（法规相关的错误不凑合）。

## Core2021

LR2021 分 HF（2.4 GHz + 850–930 MHz）与 LF（2.4 GHz + 410–510 MHz）两个 SKU， 两个变体各自带自己的 sub-G 标定表：

- HF：`tx-power-cfg-lf` 与 `tx-dbm-to-ua-reg-mode-dcdc-lf-vreg` 用 902 MHz 的参考表（Semtech Wio-LR2021 实测），`calibration-freqs` = 897.5 MHz + 2.44 GHz
- LF：用 470 MHz 的参考表，`calibration-freqs` = 433.3 / 470.3 / 498.8 MHz + 2.44 GHz（LF 变体要同时覆盖 470 与 433，所以标定点多一个）

两张表都是参考值，上板后要按 Core2021 的 PA 网络重标——验收标准见 [doc/05-roadmap.md](../../../doc/05-roadmap.md) 的 M0。

模块用晶振（不是 TCXO），所以 `tcxo-*` 保持关断；`reg-mode` 用 DC/DC。

## 构建

```shell
west build -b rp2350_zero_core2021/rp2350a/m33/hf app   # 或 .../lf
```

烧写走 BOOTSEL + UF2（`west flash --runner uf2`）或 SWD（openocd / probe-rs / jlink）。

## 分区与启动

两块板子的镜像都是 4 MB flash，分区按变体分两套（定义在 dts/dtsi 里）：

**不带 bootloader 的 hf / lf 变体**（`rp2350_zero_core2021_partitions_1m.dtsi`）：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| code | 0x000000 | 1 MB | 应用镜像（`zephyr,code-partition`） |
| storage | 0x100000 | 3 MB | NVS：根密钥、设备标识、配对记录、失控值、波特率 |

启动链：RP2350 bootrom → 应用（`west build` 的产物，`zephyr.uf2` 直接拖进去）。

**带 MCUboot 的 hf/mcuboot 与 lf/mcuboot 变体**
（`rp2350_zero_core2021_partitions_mcuboot.dtsi`）：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| mcuboot | 0x000000 | 128 KB | bootloader（实测 61.6 KB，留出 USB CDC 恢复等后续配置的余量） |
| image-0 (slot0) | 0x020000 | 832 KB | 应用镜像（签名后放这里） |
| image-1 (slot1) | 0x0F0000 | 832 KB | 升级镜像落这里，MCUboot 换槽 |
| storage | 0x1C0000 | 2.25 MB | 同上面的 NVS |

启动链：RP2350 bootrom → MCUboot（校验 slot0 的签名）→ 应用。换槽方式用
`swap using offset`（不需要 scratch 分区，升级后可以回滚）。

第一次烧写（没有调试器）：

```shell
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/hf/mcuboot app
python3 scripts/make-signed-uf2.py          # 生成应用镜像的 signed UF2
```

按住 BOOTSEL 插 USB，把 `build/mcuboot/zephyr/zephyr.uf2` 与应用镜像的
`zephyr.signed.uf2` 依次拖进 `RPI-RP2` 盘。之后升级走 MCUboot 的串口恢复
（数据口 UART1，GP8/GP9），见 [README](../../../README.md) 与
[doc/01-system.md](../../../doc/01-system.md) 2.4。

**注意**：应用目录里那份 `zephyr.uf2` 是**未签名**镜像，MCUboot 不认（而且基址是
flash 起始地址，不是 slot0）。带 bootloader 的构建只拖 `zephyr.signed.uf2`。

之后升级不用再按 BOOTSEL：主机先用 mcumgr 往数据口发命令，再让设备复位
（拔插 USB 或按复位键），bootloader 在复位后的 300 ms 窗口里接住命令就停在串口
恢复模式，上传 `zephyr.signed.bin` 即可。

只提供 Cortex-M33 这一档。`west boards` 里还会带出同一个 SoC 的 `rp2350a/hazard3` qualifier（qualifier 由 SoC 定义决定，板子改不了），但本板没有对应的 dts，用它会掉进 `stub.dts` 然后 Kconfig 报错——本板不跑 RISC-V 那两个核。
