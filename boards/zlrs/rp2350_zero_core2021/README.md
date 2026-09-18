# Waveshare RP2350-Zero + Core2021（板级定义）

微雪 RP2350-Zero（RP2350A，4 MB flash，Cortex-M33）接一颗微雪 Core2021（Semtech LR2021）。板级定义把射频接线、两路串口（CRSF 与数据口）、USB 上的三块 CDC、按键与 flash 分区固定进 devicetree；应用只认 `zlrs-*` 别名，不写引脚号。

板子叫 `rp2350_zero_core2021` 而不是 `rp2350_zero`：Zephyr 4.4 树内已经有 `waveshare/rp2350_zero`（裸板，没有射频），板名必须全局唯一；这里的差异是多一颗 Core2021 与一整套接线，所以 vendor 用自己的 `zlrs`。

**实物与各模式下的用法见 [doc/07-prototype.md](../../../doc/07-prototype.md)**；本文只写板级定义：引脚与别名、SKU 标定、分区与构建目标。

## 1. 引脚与别名

| 信号 | RP2350-Zero | DT 别名 / 节点 | 说明 |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | `lora-transceiver` = `lora_core2021` | 射频总线，`spi-max-frequency` 16 MHz |
| SPI0 CS | GP5 | 同上（`cs-gpios`） | 普通 GPIO 片选，与 Semtech 自家 shield 做法一致 |
| BUSY | GP20 | 同上（`busy-gpios`） | 模块忙，高有效，内部上拉 |
| NRESET | GP21 | 同上（`reset-gpios`） | 复位，低有效 |
| DIO8 | GP22 | 同上（`dios/dio@8`） | IRQ，高有效，内部下拉 |
| UART0 TX / RX | GP0 / GP1 | `zlrs-crsf-uart` = `uart0` | CRSF，420000，接手台/飞控 |
| UART1 TX / RX | GP8 / GP9 | `zlrs-data-uart` = `uart1` | 透传/配置口，默认 115200（NVS 可改） |
| 按键 | GP12 | `zlrs-button`、`smtc-user-button` | 对地，内部上拉；本板没焊，留给自接按键 |
| WS2812B | GP16 | `zlrs-led`、`led-strip` | 板上自带，PIO0 驱动，语义见 [doc/09-ux.md](../../../doc/09-ux.md) |
| USB-C（原生 USB） | — | `zephyr_udc0` = `usbd` | 三块 CDC：`cdc_acm_uart0`（`zlrs-crsf-cdc`）、`cdc_acm_uart1`（`zlrs-data-cdc`）、`cdc_acm_uart2`（控制台，`zephyr,console`） |
| 看门狗 | — | `watchdog0` = `wdt0` | 应用侧 `CONFIG_ZLRS_APP_WATCHDOG` 打开才武装 |

射频总线没用 Pico 默认的 GP16–19：GP16 在 RP2350-Zero 上是板载 WS2812B 的数据脚，留给状态灯。`smtc-user-button` 是 USP 官方示例要的别名，挂了它 `usp_zephyr/samples/usp/**` 就能直接在这块板上编。

每条链路有两块端点（UART + USB CDC），二选一的规则与配置模式入口见 [doc/02-platform.md](../../../doc/02-platform.md) 第 2 节，物理接口清单见 [doc/07-prototype.md](../../../doc/07-prototype.md) 第 2 节。

## 2. 两个 SKU → 四个构建目标

Core2021 的 sub-G 是**两个不同的 SKU**，PA 网络与频段都不同，所以板级分成两个变体（代码同一份，差别在 dtsi 与默认地区）：

| 构建目标 | sub-G 硬件 | 支持地区（`CONFIG_ZLRS_APP_REGION`） | 默认 |
| --- | --- | --- | --- |
| `.../rp2350a/m33/hf` | HF：850–930 MHz | FCC915(0) / EU868(1) | FCC915 |
| `.../rp2350a/m33/lf` | LF：410–510 MHz | CN470(2) / CN433(3) | CN470 |
| `.../rp2350a/m33/hf/mcuboot` | 同 hf | 同 hf | FCC915 |
| `.../rp2350a/m33/lf/mcuboot` | 同 lf | 同 lf | CN470 |

带 `/mcuboot` 的两个变体多一个 bootloader 与两槽固件分区，构建要加 `--sysbuild`（见第 4 节）。**板子只有这两种形态**：不带变体的 `rp2350_zero_core2021/rp2350a/m33` 没有对应的 dts，用它构建会掉进 `stub.dts` 然后 Kconfig 报错。

```shell
west build -b rp2350_zero_core2021/rp2350a/m33/hf app   # 868 / 915
west build -b rp2350_zero_core2021/rp2350a/m33/lf app   # 470 / 433
```

`west boards` 里还会带出同一个 SoC 的 `rp2350a/hazard3` qualifier（qualifier 由 SoC 定义决定，板子改不了），但本板没有对应的 dts，用它同样会掉进 `stub.dts`——本板不跑 RISC-V 那两个核。

### SKU 声明

板子通过 DT 节点 `zlrs_subg_band`（`compatible = "zlrs,subg-band"`，绑定见 `dts/bindings/zlrs,subg-band.yaml`）声明自己是哪种 sub-G 硬件：`band = <0>` LF、`<1>` HF。应用启动时校验"地区 vs 本机硬件"，不匹配就**红灯告警并且不发射频**（法规相关的错误不凑合）。

这个值**不要**写进 `/chosen`：Zephyr 的 DT 宏只导出 `zephyr,` 前缀的 chosen 属性，自定义前缀读不到，会静默退回默认值 HF——LF 板于是永远过不了校验（上板第一次就是这个现象，见 [doc/10-roadmap.md](../../../doc/10-roadmap.md) 的 M1）。

## 3. Core2021 与标定

- 模块用**晶振**（不是 TCXO），所以 `tcxo-*` 保持关断；`reg-mode` 用 DC/DC；`lf-clk` 用内部 RC。
- 板级公共部分给 `tx-power-offset = 0` 与 `rx-boost-cfg = 7`（最大 RX boost）。
- 两个变体各带自己的 sub-G 标定表：
  - **HF**：`tx-power-cfg-lf` 与 `tx-dbm-to-ua-reg-mode-dcdc-lf-vreg` 用 902 MHz 的参考表（Semtech Wio-LR2021 实测），`calibration-freqs` = 897.5 MHz + 2.44 GHz。
  - **LF**：用 470 MHz 的参考表，`calibration-freqs` = 433.3 / 470.3 / 498.8 MHz + 2.44 GHz（LF 要同时覆盖 470 与 433，所以标定点多一个）。
- 两张表都是**参考值**，上板后要按 Core2021 的 PA 网络重标，验收标准见 [doc/10-roadmap.md](../../../doc/10-roadmap.md) 的 M0。
- 2.4 GHz 那一路两个 SKU 相同（同一颗 LR2021 分时双频），标定频点里都带 2.441 GHz。

## 4. 分区与启动

4 MB flash，两套布局：

**不带 bootloader 的 hf / lf**（`rp2350_zero_core2021_partitions_1m.dtsi`）：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| code | 0x000000 | 1 MB | 应用镜像（`zephyr,code-partition`） |
| storage | 0x100000 | 3 MB | NVS：根密钥、设备标识、配对记录、失控值、波特率 |

启动链：RP2350 bootrom → 应用（`zephyr.uf2` 直接拖进 ROM 的 `RPI-RP2` 盘）。

**带 MCUboot 的 hf/mcuboot 与 lf/mcuboot**（`rp2350_zero_core2021_partitions_mcuboot.dtsi`）：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| mcuboot | 0x000000 | 128 KB | bootloader（实测 61.6 KB，留余量给 USB CDC 恢复） |
| image-0（slot0） | 0x020000 | 832 KB | 应用镜像（签名后放这里，`zephyr,code-partition`） |
| image-1（slot1） | 0x0F0000 | 832 KB | 升级镜像落这里，MCUboot 换槽 |
| storage | 0x1C0000 | 2.25 MB | 同上面的 NVS |

启动链：RP2350 bootrom → MCUboot（校验 slot0 的签名）→ 应用。换槽方式 `swap using offset`（不需要 scratch 分区，升级后可以回滚）。启动链、镜像确认与恢复入口的机制见 [doc/02-platform.md](../../../doc/02-platform.md) 第 5 节。

不带 bootloader 的变体烧写走 BOOTSEL + UF2（`west flash --runner uf2`）或 SWD（openocd / probe-rs / jlink）。带 MCUboot 的第一次烧写要**两轮 BOOTSEL**：

```shell
west build --sysbuild -b rp2350_zero_core2021/rp2350a/m33/hf/mcuboot app
python3 scripts/make-signed-uf2.py
```

1. 按住 BOOTSEL 插 USB，拖 `build/mcuboot/zephyr/zephyr.uf2`；
2. 再按住 BOOTSEL 插一次 USB，拖应用镜像的 `zephyr.signed.uf2`。

bootloader 跑起来以后 ROM 的 U 盘模式不会再回来，两轮省不掉。**注意**：应用目录里那份 `zephyr.uf2` 是未签名镜像，MCUboot 不认（基址是 flash 起始地址而非 slot0），带 bootloader 的构建只拖 `zephyr.signed.uf2`。

之后升级走 MCUboot 串口恢复，**只走 USB**（数据口那块 CDC，见 [doc/02-platform.md](../../../doc/02-platform.md) 第 5 节）；GP8/GP9 那路 UART 仍是应用自己的数据口，升级用不到。bootloader 模式的 USB 只有两块 CDC（第 1 块恢复、第 2 块控制台），CRSF 那块被 `app/sysbuild/mcuboot.overlay` 删掉，所以脚本按 `/dev/serial/by-id/...MCUBOOT...` 找口，不能沿用应用模式的 `ttyACM1`：

```shell
python3 scripts/zlrs-recovery.py --port /dev/ttyACM1   # 应用侧触发进恢复
python3 scripts/zlrs-update.py --port /dev/ttyACM1     # 触发 + 上传 + 等应用回来
```

## 5. 板级默认值

`Kconfig.defconfig` 里给所有变体设的默认值：

- `CONFIG_BUILD_OUTPUT_HEX` / `BUILD_OUTPUT_UF2`：产出 hex 与可直接拖的 uf2。
- 控制台挂在 USB 第三块 CDC，所以板级默认打开 `USB_DEVICE_STACK_NEXT` 与 `USBD_CDC_ACM_CLASS`。**`CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` 保持关闭**：那个辅助实现只注册第一块 CDC，应用与 bootloader 要的是 `lib/usb` 自己建 context、注册全部 CDC（原因见 [doc/02-platform.md](../../../doc/02-platform.md) 第 3 节）。
- `USE_DT_CODE_PARTITION=y`：镜像链接进 devicetree 里的 code 分区，长过 1 MB 直接编译报错，不会上板后才发现压掉 storage。
- `CONFIG_UART_LINE_CTRL`、`UART_INTERRUPT_DRIVEN`、`GPIO`、`RESET`、`CLOCK_CONTROL`、`SERIAL`、`UART_CONSOLE` 等外设常开。
