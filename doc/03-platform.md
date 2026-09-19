# 硬件平台

本文写 zlrs 跑在什么上：目标形态、板级、构建目标、工具链、镜像布局与烧录。端口与工作模式见 [08-firmware](08-firmware.md)，实物见 [04-prototype](04-prototype.md) 与 [05-carrier](05-carrier.md)。

## 1. 目标形态

| 形态 | 内容 |
| --- | --- |
| **原型** | 微雪 RP2350-Zero + 微雪 Core2021 两块模块用线连起来，对外只有 USB-C，见 [04-prototype](04-prototype.md) |
| **载板** | 把两模块的连线做成 PCB，对外给 USB-C、按键与两路 6P 串口，见 [05-carrier](05-carrier.md) |
| **轻量版** | 裁掉 USB 与数据口，只留控制链路，见 [08-firmware](08-firmware.md) 第 4 节 |

三个形态（原型 / 载板 / 轻量版）共用同一份固件与同一套板级定义，差别只在 devicetree 与 Kconfig。

## 2. 板级

| 项目 | 选择 |
| --- | --- |
| 射频 | Semtech LR2021（QFN32），SPI + BUSY + DIO；参数见 [06-radio](06-radio.md) |
| 主控 | 微雪 RP2350-Zero：RP2350A（Cortex-M33）、4 MB QSPI flash、原生 USB、板载 WS2812B |
| 射频模块 | 微雪 Core2021，sub-G 分 HF（850–930 MHz）与 LF（410–510 MHz）两个 SKU |
| 板级定义 | `boards/zlrs/rp2350_zero_core2021/`：引脚、别名、SKU 标定表、flash 分区 |
| Zephyr | v4.4.0 |
| 射频栈 | `usp_zephyr` + `usp` v1.1.2-feature-202604（RAC/RALF 层） |
| 仓库拓扑 | T2：zlrs 自己当 manifest 仓库并存放应用 |

引脚、别名与分区的权威定义见[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md)。

## 3. 构建目标

四个目标，两个 SKU × 是否带 bootloader：

| 构建目标 | 说明 |
| --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | HF SKU（868 / 915），应用直接放 flash |
| `rp2350_zero_core2021/rp2350a/m33/lf` | LF SKU（470 / 433），应用直接放 flash |
| `.../m33/hf/mcuboot` | 同上，多一个 MCUboot 与两槽分区，构建要加 `--sysbuild` |
| `.../m33/lf/mcuboot` | 同上 |

`west boards` 里还会带出同一个 SoC 的 `rp2350a/hazard3` qualifier，本板没有对应的 dts（不跑 RISC-V 核）。

## 4. 镜像布局

**不带 bootloader**：code 1 MB + storage 3 MB（NVS：根密钥、设备标识、配对记录、失控值、波特率）。

**带 MCUboot**：mcuboot 128 KB + slot0 832 KB + slot1 832 KB + storage 2.25 MB；签名镜像落在 slot0（flash 地址 `0x10020000`）。启动链 RP2350 bootrom → MCUboot（验签）→ slot0，换槽用 `swap using move`（`swap using offset` 在这块板上换得进去、回不来），升级失败可回滚。

具体偏移与大小见[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md#4-分区与启动)；升级流程与恢复入口见 [08-firmware](08-firmware.md) 第 6 节。

## 5. 工具链与构建

工作区用本仓库的 `west.yml` 建（Zephyr v4.4.0、usp_zephyr、usp），工具链固定 Zephyr SDK 1.0.0（发行版 `arm-none-eabi-gcc` 的 newlib 没开 retargetable locking，会撞静态断言）：

```shell
scripts/setup-zephyr.sh        # 首次约 5 GB
source scripts/zephyr-env.sh   # 每个新 shell 一次
zlrs-build                     # = west build -b $ZLRS_BOARD $ZLRS_APP
```

主机侧测试不碰工作区：`make -C tests/host run` 只编译 `lib/` 里那同一份源码。命令与依赖的完整列表见 [README](../README.md)。

## 6. 烧录

- 不带 bootloader：BOOTSEL + UF2（`west flash --runner uf2`）或 SWD。
- 带 MCUboot：第一次两轮 BOOTSEL（先 bootloader，再 `zephyr.signed.uf2`）；之后走串口恢复，一条命令 `python3 scripts/zlrs-update.py --port /dev/ttyACM1`。

恢复口、镜像确认与回滚的机制见 [08-firmware](08-firmware.md) 第 6 节。
