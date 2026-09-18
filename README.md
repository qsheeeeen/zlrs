# zlrs

基于 Zephyr、射频芯片为 Semtech LR2021 的点对点链路：**同时支持数传与控制两种链路**。TX 与 RX 是同一份固件，角色由 Kconfig 选。

- 三种形态：纯 2.4G 高速控制（竞速）、纯 2.4G 高速数传、双频混合（sub-G 控制 + 2.4G 数传）——见 [doc/01-product.md](doc/01-product.md)。
- 单颗射频分时双频：默认 sub-GHz 跑控制、2.4 GHz 跑数传。
- 一份固件适配多种硬件：角色、频段、USB 之类的特性由 Kconfig 与 devicetree 裁剪。
- 自研协议，不与 ExpressLRS 兼容；关系见 [doc/02-vs-elrs.md](doc/02-vs-elrs.md)。

射频驱动来自 Semtech 的 USP，不在本仓库。文档索引与阅读路径见 [doc/README.md](doc/README.md)。

## 硬件

现在的实物是两模块原型：微雪 RP2350-Zero（RP2350A，4 MB flash）+ 微雪 Core2021（LR2021）用线连起来，对外只有 USB-C；按键用杜邦线短接 GP12 模拟。见 [doc/04-prototype.md](doc/04-prototype.md)。载板形态见 [doc/05-carrier.md](doc/05-carrier.md)，引脚、SKU 标定与分区见[板级 README](boards/zlrs/rp2350_zero_core2021/README.md)。

Core2021 的 sub-G 是两个 SKU，所以板级是两个变体：

| 构建目标 | sub-G | 可用地区（运行期可配） |
| --- | --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | HF：850–930 MHz | FCC915 / EU868 |
| `rp2350_zero_core2021/rp2350a/m33/lf` | LF：410–510 MHz | CN470 / CN433 |

## 构建与烧写

```shell
scripts/setup-zephyr.sh        # 首次约 5 GB
source scripts/zephyr-env.sh   # 每个新 shell 一次
zlrs-build                     # = west build -b $ZLRS_BOARD $ZLRS_APP
cd "$ZLRS_WS" && west flash    # 默认 openocd，也可 --runner uf2/probe-rs/jlink
```

带 MCUboot 的构建（推荐，之后升级不用按 BOOTSEL）：

```shell
zlrs-build-mcuboot                # = west build --sysbuild -b $ZLRS_BOARD/mcuboot $ZLRS_APP
python3 scripts/make-signed-uf2.py
```

第一次烧写要两轮 BOOTSEL：先拖 `build/mcuboot/zephyr/zephyr.uf2`，再进一次 BOOTSEL 拖 `build/app/zephyr/zephyr.signed.uf2`。之后一条命令升级：

```shell
python3 scripts/zlrs-update.py --port /dev/ttyACM1
```

两种模式在 USB 上自报的名字不同（应用 `ZLRS`、bootloader `ZLRS MCUBOOT`），脚本按 `/dev/serial/by-id/...` 找口，不写死 `ttyACM*`。启动链、恢复入口与回滚见 [doc/08-firmware.md](doc/08-firmware.md) 第 6 节。

`scripts/zephyr-env.sh` 导出：`ZLRS_WS`（工作区根）、`ZLRS_APP`、`ZLRS_BOARD`（默认 hf，LF 用 `.../lf`）、`ZLRS_SDK_DIR`（默认 `~/zephyr-sdk-1.0.0`）。

## 配置

USB 上有三块 CDC：**CRSF、数据、配置**。配置工具在配置 CDC 上发"进配置模式"命令，设备进入后射频完全停，退出后恢复。所有用户参数都在运行期配置，不用重编固件：

```shell
python3 scripts/zlrs-config.py --self-test
python3 scripts/zlrs-config.py --port /dev/ttyACM2 status
python3 scripts/zlrs-config.py --port /dev/ttyACM2 set-phrase "correct horse battery staple"
python3 scripts/zlrs-config.py --port /dev/ttyACM2 set-region 3
python3 scripts/zlrs-config.py --port /dev/ttyACM2 set-failsafe @default
python3 scripts/zlrs-config.py --port /dev/ttyACM2 exit
```

命令集见 [doc/10-security.md](doc/10-security.md) 第 10 节，固件行为见 [doc/08-firmware.md](doc/08-firmware.md) 第 3 节。

### 编译期配置

角色与链路形态决定固件跑哪套时序，属于编译期配置（其余用户参数都在运行期配）：

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_ROLE_TX` | y | 关掉就是 RX；TX 与 RX 是同一份固件 |
| `CONFIG_ZLRS_APP_BAND_ROLE_SUBG_CONTROL` / `..._SUBG_DATA` | 前者 | 双频混合（sub-G 控制 + 2.4G 数传）或纯 2.4G 竞速形态 |
| `CONFIG_ZLRS_APP_LINK_CONTROL` / `CONFIG_ZLRS_APP_LINK_DATA` | 都开 | 关掉一条链路只跑另一条 |
| `CONFIG_ZLRS_APP_CONTROL_RATE_HZ` | 150 | 控制帧率档（50–500），实际值会被地区占空比与空中时间压档 |
| `CONFIG_ZLRS_APP_RACING_CONTROL_RATE_BPS` / `..._HZ` | 2.6 Mbps / 500 Hz | 竞速形态的控制链路参数 |
| `CONFIG_ZLRS_APP_CONTROL_MODULATION_FLRC` / `..._LORA` | 前者 | 控制链路用宽带 FLRC 还是 LoRa 远距档；LoRa 档速率由 `CONFIG_ZLRS_APP_LORA_CONTROL_RATE_HZ` 配置 |
| `CONFIG_ZLRS_APP_DATA_ARQ` | 见 [doc/13-roadmap.md](doc/13-roadmap.md) | 数传可靠重传；目标默认开启 |
| `CONFIG_ZLRS_APP_RATE_ADAPT` | n | 按 LQ 自动升降速率档；阈值待实测标定 |
| `CONFIG_ZLRS_APP_DYNAMIC_POWER` | y | 动态功率；下限 `CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM` |
| `CONFIG_ZLRS_APP_CONFIG_MODE` | y | 配置模式；调试构建另有数据口魔数与 shell 入口（[doc/12-debug.md](doc/12-debug.md)） |
| `CONFIG_ZLRS_APP_ROOT_KEY_HEX` | 空 | 台架测试用：直接内置一个根密钥，省去配置步骤（正式流程仍走配置工具） |

## 能编什么

| 产物 | 命令 | 说明 |
| --- | --- | --- |
| 固件（TX 或 RX） | `zlrs-build` | 默认目标 `.../m33/hf`（LF 用 `.../lf`） |
| 固件 + MCUboot | `zlrs-build-mcuboot` | 加 `--sysbuild`，多一个 bootloader 与两槽分区 |
| 主机侧测试 | `make -C tests/host run` | 协议核心、链路层、CRSF 与配置协议；不需要硬件与工作区 |
| 上板自检 | `python3 scripts/zlrs-bringup.py` | 三块 CDC、配置模式、配置协议读写、NVS 持久化 |
| 配置工具 | `python3 scripts/zlrs-config.py` | 读状态、写参数、触发配对、退出 |
| 升级脚本 | `python3 scripts/zlrs-update.py` | 触发进恢复 + 上传签名镜像 + 等应用回来 |

官方示例（Semtech 的 `packet_error_rate_flrc`）可以直接跑在这块板上，用来单独验证工具链与射频栈。

## 仓库结构

```
app/                  Zephyr 应用：USP/RAC 移植、端口层、超帧主循环、NVS 持久化
boards/zlrs/          板级定义（RP2350-Zero + Core2021）
lib/proto/            协议核心：帧编解码、分片重组、跳频表、密钥派生与标签、
                      遥测载荷、调制档与空中时间模型（FLRC / LoRa）
lib/link/             链路层（纯 C）：状态机、配对与会话、超帧时隙、射频抽象接口、
                      可靠重传与速率自适应策略
lib/crsf/             CRSF 端口编解码（纯 C）
lib/config/           配置协议（纯 C）：帧格式 + 命令分发
lib/region/           地区配置层（纯 C）：信道表、功率上限、占空比记账
lib/led/              状态灯模式引擎（纯 C）
lib/usb/              USB 设备栈（usbd context + devicetree 里的全部 CDC）
include/zlrs/         对外头文件
tests/host/           主机侧测试与双节点仿真
scripts/              工作区、配置、升级与自检脚本
doc/                  设计文档（索引见 doc/README.md）
zephyr/module.yml     本仓库作为 Zephyr 模块的入口
```

`lib/` 与 `include/` 不依赖 Zephyr，协议核心与链路层在主机上编译同一份源码；设备侧只多一个射频移植层。射频侧的一切（SPI 时序、校准、频段、收发调度）由 `usp_zephyr` 承担。

## 文档

索引与阅读路径见 [doc/README.md](doc/README.md)。四块：产品设计（需求）、硬件、软件、开发。

| 文档 | 内容 |
| --- | --- |
| [01-product](doc/01-product.md) | 定位、三种形态与场景、距离 × 速率目标、性能指标（对标 ELRS）、边界 |
| [02-vs-elrs](doc/02-vs-elrs.md) | 与 ExpressLRS 的关系：参考与对立 |
| [03-platform](doc/03-platform.md) | 硬件平台、构建目标、镜像布局、烧录 |
| [04-prototype](doc/04-prototype.md) | 原型实物与各模式用法 |
| [05-carrier](doc/05-carrier.md) | 载板 |
| [06-radio](doc/06-radio.md) | LR2021 参数摘要 |
| [07-architecture](doc/07-architecture.md) | 两条链路、分时、时序预算、分层、线程 |
| [08-firmware](doc/08-firmware.md) | 端口与模式、运行期配置、裁剪、升级、错误处理约定 |
| [09-protocol](doc/09-protocol.md) | 帧格式、状态机、会话、同步、可靠传输、跳频与法规 |
| [10-security](doc/10-security.md) | 身份密钥、配对、威胁模型、配置协议 |
| [11-ux](doc/11-ux.md) | 按键与状态灯 |
| [12-debug](doc/12-debug.md) | debug overlay、调试开关与上板调试方法（开发者） |
| [13-roadmap](doc/13-roadmap.md) | 待办与验收标准 |

## 依赖

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Zephyr | v4.4.0 | 由 [west.yml](west.yml) 固定 |
| usp_zephyr / usp | v1.1.2-feature-202604 | Semtech 射频栈（RAC/RALF） |
| Zephyr SDK | 1.0.0 | 装 `arm-zephyr-eabi` 一个工具链 |
| 密码学 | Zephyr PSA Crypto | 设备侧 KDF 与每包 tag；主机测试用等价的纯 C 实现 |

## 构建要点

1. **板子在本仓库里**：`zephyr/module.yml` 把仓库根同时作为 `board_root` 与 `dts_root`，不需要 `--shield`。
2. **换硬件只改 devicetree**：应用认别名（`lora-transceiver`、`zlrs-crsf-uart` / `-cdc`、`zlrs-data-uart` / `-cdc`、`zlrs-button`、`zlrs-led`），别名缺失的端口自动裁掉。
3. **工具链固定用 Zephyr SDK**：发行版的 `arm-none-eabi-gcc` 会撞静态断言。
4. **镜像不许越过 code 分区**：越界是编译错误。
5. **主机侧测试不碰工作区**：`tests/host` 只编译 `lib/` 里那同一份源码。

## 测试

`tests/host` 用假射频与假时钟跑仿真：配对与会话、跳频锁步、防重放、跨帧透传与丢包恢复、失联与重连、同场多节点、畸形输入、时间戳与序号回绕，以及每个调试开关"关掉之后照常工作"。默认带 ASan + UBSan。

CI 跑主机测试与各脚本的 self-test（[.github/workflows/build.yml](.github/workflows/build.yml)）；固件构建单独一条流水线（[firmware.yml](.github/workflows/firmware.yml)）。
