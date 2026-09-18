# zlrs

基于 Zephyr、射频芯片为 Semtech LR2021 的点对点链路：一条 sub-GHz 控制链路（CRSF 遥控通道 + 遥测）加一条 2.4 GHz 数据链路（UART 串口透传）。TX 与 RX 是同一份固件，角色由 Kconfig 选。

- **主诉求是串口透传**：两端的 UART 字节经空口互传；控制链路同时保证遥控可用。两条链路各跑一份相同的状态机，互不阻塞。
- **单颗射频分时双频**：默认 sub-GHz 跑控制、2.4 GHz 跑透传；频段与链路角色可配，竞速场景可以对调（[doc/01-system.md](doc/01-system.md) 3.2）。
- **一份固件适配多种硬件**：角色、频段、USB 之类的特性由 Kconfig 与 devicetree 裁剪，不写 `#ifdef` 分支。
- **自研协议**：不与 ExpressLRS 兼容；控制距离目标 2 km；不做图传；不做产线密钥注入。

仓库包含协议核心、链路层、固件（射频链路、CRSF 端口、串口透传、配置模式、配对持久化）与板级定义；射频驱动来自 Semtech 的 USP，不在本仓库。接下来做什么见 [doc/05-roadmap.md](doc/05-roadmap.md)。

## 硬件

现在的实物是两模块原型：微雪 RP2350-Zero（RP2350A，4 MB flash，Cortex-M33）+ 微雪 Core2021（LR2021）用线连起来，**对外只有 USB-C 一个口**。物理接口、模式用法与板内连线见 [doc/09-prototype.md](doc/09-prototype.md)；引脚表、SKU 标定与 flash 分区见 [板级 README](boards/zlrs/rp2350_zero_core2021/README.md)。

Core2021 的 sub-G 是**两个 SKU**（PA 网络不同），所以板级是两个变体，代码同一份：

| 构建目标 | sub-G | 可用地区（`CONFIG_ZLRS_APP_REGION`） |
| --- | --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | HF：850–930 MHz | FCC915(0) / EU868(1) |
| `rp2350_zero_core2021/rp2350a/m33/lf` | LF：410–510 MHz | CN470(2) / CN433(3) |

两个变体各带自己的 sub-G 标定表与标定频点；地区与本机 SKU 不匹配时固件**红灯告警且不发射频**。2.4 GHz 那一路两个 SKU 相同——LR2021 是单颗分时双频，不需要两颗模块。

## 能编什么

| 产物 | 命令 | 说明 |
| --- | --- | --- |
| 固件（TX 或 RX） | `zlrs-build` | 默认目标 `rp2350_zero_core2021/rp2350a/m33/hf`（LF SKU 用 `.../m33/lf`） |
| 固件 + MCUboot | `zlrs-build-mcuboot` | 目标 `.../m33/hf/mcuboot`（LF 用 `.../m33/lf/mcuboot`），`west build --sysbuild` 把 bootloader 一起编出来 |
| 主机侧测试 | `make -C tests/host run` | 协议核心、链路层（含 ARQ 与占空比）、CRSF 与配置协议；不需要硬件与工作区 |
| 上板自检 | `python3 scripts/zlrs-bringup.py` | "不依赖射频"那一档：三块 CDC、数据口魔数进配置模式、配置协议读写、NVS 持久化 |
| 配置工具 | `python3 scripts/zlrs-config.py` | 读状态、写短语/密钥、写失控值、触发配对、改波特率；`--self-test` 不接设备 |
| 升级脚本 | `python3 scripts/zlrs-update.py` | 触发进恢复 + 上传签名镜像 + 等应用回来；`--self-test` 不接设备 |

官方示例（Semtech 的 `packet_error_rate_flrc`）可以直接跑在这块板上，用来单独验证工具链与射频栈：

```shell
cd "$ZLRS_WS"
west build -b "$ZLRS_BOARD" usp_zephyr/samples/usp/sdk/packet_error_rate_flrc \
  -- -DEXTRA_CFLAGS="-DROLE=RECEIVER"
```

## 上手

### 1. 搭工作区

本仓库在 west 里是 manifest 仓库（T2 拓扑）：清单 [west.yml](west.yml) 固定 Zephyr v4.4.0、usp_zephyr 与 usp v1.1.2-feature-202604，工作区默认建在仓库的上一级目录。

```shell
scripts/setup-zephyr.sh        # 首次约 5 GB
source scripts/zephyr-env.sh   # 每个新 shell 一次
```

工具链用 Zephyr SDK 1.0.0（`ZLRS_SDK_DIR` 可换）：脚本会检查，缺了会打印安装命令。

`scripts/zephyr-env.sh` 导出这些变量：

| 变量 | 默认值 |
| --- | --- |
| `ZLRS_WS` | 仓库的上一级目录（west 工作区根） |
| `ZLRS_APP` | 仓库里的 `app/` |
| `ZLRS_BOARD` | `rp2350_zero_core2021/rp2350a/m33/hf`（LF SKU 用 `.../lf`） |
| `ZLRS_SDK_DIR` | `~/zephyr-sdk-1.0.0` |

### 2. 构建与烧写

```shell
zlrs-build                     # = west build -b $ZLRS_BOARD $ZLRS_APP
cd "$ZLRS_WS" && west flash    # 默认 openocd，也可 --runner uf2/probe-rs/jlink
```

kernel log 与 shell 在 USB 的第三个 CDC 上（Linux 下 `/dev/ttyACM2`）。

不带 bootloader 的变体也可以按住 BOOTSEL 上电，把 `$ZLRS_WS/build/zephyr/zephyr.uf2` 拖进 `RPI-RP2` 盘。

#### 带 MCUboot（推荐）

换成 `/mcuboot` 目标就把 bootloader 一起编进来，之后升级不用再按 BOOTSEL：

```shell
zlrs-build-mcuboot                # = west build --sysbuild -b $ZLRS_BOARD/mcuboot $ZLRS_APP
python3 scripts/make-signed-uf2.py
```

**第一次烧写要两轮 BOOTSEL**：bootloader 一旦跑起来，ROM 的 `RPI-RP2` 盘就交不回来了（MCUboot 只有串口恢复），所以得再进一次 BOOTSEL 才能写应用镜像。flash 起始处只要有可启动镜像，复位后跑的就是它。

1. 按住 BOOTSEL 插 USB，拖 `build/mcuboot/zephyr/zephyr.uf2`；
2. 再按住 BOOTSEL 插一次 USB，拖 `build/app/zephyr/zephyr.signed.uf2`（签名镜像落在 slot0，flash 地址 `0x10020000`）。

之后升级不用碰设备：应用侧把 boot mode 标志写进 retained RAM 并重启，MCUboot 读到就停在串口恢复里等上传。一条命令做完（触发 + 等恢复口 + `smpmgr upgrade` + 等应用回来）：

```shell
python3 scripts/zlrs-update.py --port /dev/ttyACM1        # 镜像默认取 build/app/zephyr/zephyr.signed.bin
```

两种模式在 USB 上自报的名字不同：应用是 `ZLRS`、bootloader 是 `ZLRS MCUBOOT`，脚本按 `/dev/serial/by-id/...` 找口，别写死 `ttyACM*` 编号。`--client none` 只打印要执行的命令，`--no-trigger` 用于设备已经在恢复模式的情况，`--self-test` 不接设备也能跑。

启动链、分区、回滚判据与恢复入口的完整说明见 [doc/01-system.md](doc/01-system.md) 2.4，分区数字见 [板级 README](boards/zlrs/rp2350_zero_core2021/README.md)。

### 3. 角色、密钥与配对

- **角色**：默认 TX；RX 用 `-- -DCONFIG_ZLRS_APP_ROLE_TX=n` 关掉。
- **根密钥**：固件默认没有根密钥，上电后不参与收发。正式做法是配置模式写入（见下）；台架测试也可以临时给一个：

  ```shell
  ZLRS_BENCH_KEY=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
  west build -b "$ZLRS_BOARD" "$ZLRS_APP" \
    -- -DCONFIG_ZLRS_APP_ROOT_KEY_HEX=\"$ZLRS_BENCH_KEY\"
  ```

- **配对**：运行中长按按键（默认 1 秒）触发三次握手；配对记录存 NVS，重启后两条链路都自动恢复，不必重新配对。换绑也是长按重新配对，新记录顶掉旧的。

### 4. 配置模式

配置模式把数据口从透传切到配置协议，无操作 300 秒或收到 `EXIT` 后回到正常收发。入口有三条，都不需要专用控制口：

| 入口 | 开关 | 说明 |
| --- | --- | --- |
| 开机时按住按键 | `CONFIG_ZLRS_APP_CONFIG_MODE`（默认开） | 按键时机区分"配置"与"配对"，见 [doc/06-ui.md](doc/06-ui.md) |
| 数据口魔数 | `CONFIG_ZLRS_APP_CONFIG_MAGIC`（默认关） | 数据口收到 16 字节 `zlrs!configmode!`；给没有按键的板子用 |
| 控制台 shell | `CONFIG_ZLRS_APP_SHELL`（默认关） | 控制台 CDC 上敲 `zlrs config`；另有 `zlrs status` / `ports` / `recovery` |

三个入口进的是同一个会话，都在主循环里跑：会话期间超帧暂停，不会边透传边配置。数据口有两块端点（UART1 与 USB 数据 CDC），主机把它打开就走 USB（[doc/01-system.md](doc/01-system.md) 2.1）。

```shell
python3 scripts/zlrs-config.py --self-test                    # 不接设备，校验组帧
python3 scripts/zlrs-config.py --port /dev/ttyACM1 status
python3 scripts/zlrs-config.py --port /dev/ttyACM1 set-phrase "correct horse battery staple"
python3 scripts/zlrs-config.py --port /dev/ttyACM1 set-failsafe @default
python3 scripts/zlrs-config.py --port /dev/ttyACM1 exit
```

`--dry-run` 只打印要发的帧，方便对着抓包比对。帧格式与安全性质见 [doc/02-security.md](doc/02-security.md) 第 10 节。

### 5. 地区、速率与链路策略

常用 Kconfig（语义与取值理由见 [doc/03-protocol.md](doc/03-protocol.md)）：

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_REGION` | 随变体 | 0 = FCC915、1 = EU868（HF）；2 = CN470、3 = CN433（LF）。决定频段、功率上限、速率上限与占空比 |
| `CONFIG_ZLRS_APP_CONTROL_RATE_HZ` | 150 | 控制帧率档（50–500）。实际生效值会被地区占空比与空中时间压档 |
| `CONFIG_ZLRS_APP_CONTROL_MODULATION_LORA` | 关 | 控制链路改跑 LoRa 远距档；数据链路仍留在 2.4G 跑 FLRC |
| `CONFIG_ZLRS_APP_BAND_ROLE_SUBG_DATA` | 关 | 竞速模式：控制链路独占 2.4G，数据链路让到 sub-G |
| `CONFIG_ZLRS_APP_DATA_ARQ` | 关 | 透传改走 ARQ（序号 + 累计 ACK + 重传）；默认 best-effort |
| `CONFIG_ZLRS_APP_RATE_ADAPT` | 关 | 按 LQ 自动升降速率档；阈值待实测标定 |
| `CONFIG_ZLRS_APP_DYNAMIC_POWER` | 开 | 按对端回报的 RSSI/LQ 升降发射功率；下限 `CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM` |
| `CONFIG_ZLRS_APP_RESYNC_PERIOD_MS` | 1000 | 连接态周期 `BEACON`：跳频重同步 + 对端重启后自动重连；0 = 关 |

### 6. 上板调试开关

第一次上电不要一次把所有东西都打开：每项复杂功能都有自己的开关，从最简形态逐层往上验证，关掉的项开机会打一条 `bring-up:` 警告。完整表格与推荐顺序见 [doc/01-system.md](doc/01-system.md) 2.3；`CONFIG_ZLRS_APP_RADIO=n` 还能把射频整块关掉（Core2021 还没接上时用这个）。

```shell
# 最简形态：不带 tag、不跳频、不握手、不发遥测
west build -b "$ZLRS_BOARD" "$ZLRS_APP" -- \
  -DCONFIG_ZLRS_APP_SECURITY_PROFILE_P0=y \
  -DCONFIG_ZLRS_APP_SECURITY_PROFILE_P1=n \
  -DCONFIG_ZLRS_APP_FHSS=n \
  -DCONFIG_ZLRS_APP_SESSION_HANDSHAKE=n \
  -DCONFIG_ZLRS_APP_TELEMETRY=n
```

每个开关都有一条主机侧用例断言"关掉之后链路照常工作"（`make -C tests/host run`）。

## 仓库结构

```
app/                  Zephyr 应用：USP/RAC 移植、串口透传、NVS 持久化、超帧主循环
boards/zlrs/          板级定义（RP2350-Zero + Core2021）
lib/proto/            协议核心：帧编解码、分片重组、跳频表、密钥派生与标签、
                      遥测载荷、调制档与空中时间模型（FLRC / LoRa）
lib/link/             链路层（纯 C）：状态机、配对与会话、超帧时隙、射频抽象接口、
                      ARQ profile、速率自适应策略（档位 = 调制 + 参数）
lib/crsf/             CRSF 端口编解码（纯 C）：RC 通道帧与链路统计帧
lib/config/           配置协议（纯 C）：帧格式 + 命令分发（落盘通过回调）
lib/region/           地区配置层（纯 C）：信道表、功率上限、占空比记账
lib/led/              状态灯模式引擎（纯 C）：状态 → 颜色与节奏
lib/usb/              USB 设备栈（usbd context + devicetree 里的全部 CDC）
include/zlrs/         对外头文件
tests/host/           主机侧测试与双节点仿真
scripts/              工作区、配置、升级与自检脚本
doc/                  设计文档（见下面的文档地图）
zephyr/module.yml     本仓库作为 Zephyr 模块的入口（板级定义也挂在这里）
```

分层原则：`lib/` 与 `include/` 不依赖 Zephyr，协议核心与链路层在主机上编译同一份源码，设备侧只多一个射频移植层（`app/src/rac_port.c`）。射频侧的一切——SPI 时序、校准、频段、收发调度——都由 `usp_zephyr` 承担。宿主/设备两边的密码学实现对拍见 [doc/02-security.md](doc/02-security.md) 第 8 节。

## 文档地图

每份文档只拥有一类事实，其它地方只链接不复述：

| 文档 | 拥有 |
| --- | --- |
| [doc/01-system.md](doc/01-system.md) | 目标与边界、平台基线、端口与工作模式的设计、特性裁剪、上板调试开关、启动链与升级、系统架构、时序预算、软件分层、线程模型、错误处理 |
| [doc/02-security.md](doc/02-security.md) | 身份与密钥、配对与会话的安全性质、威胁模型、密码学参数、配置通道与配置协议帧 |
| [doc/03-protocol.md](doc/03-protocol.md) | 空中帧格式、包类型、链路状态机、会话握手、超帧、速率档与自适应、跳频与法规 |
| [doc/04-lr2021.md](doc/04-lr2021.md) | LR2021 的速率、灵敏度、切换时序、SPI/IRQ 与引脚摘要 |
| [doc/05-roadmap.md](doc/05-roadmap.md) | 里程碑、进度（已做 / 待验收 / 待做）与验收标准 |
| [doc/06-ui.md](doc/06-ui.md) | 按键语义与状态灯的颜色/节奏 |
| [doc/07-carrier.md](doc/07-carrier.md) | 载板 v1（草稿）：接插件、线序、电源与机械 |
| [doc/08-vs-elrs.md](doc/08-vs-elrs.md) | 与 ExpressLRS 的事实对比、差距与差异化 |
| [doc/09-prototype.md](doc/09-prototype.md) | 原型实物：模块、对外接口、板内连线、模式 × 接口用法 |
| [板级 README](boards/zlrs/rp2350_zero_core2021/README.md) | 板级定义：引脚与别名、两个 SKU 的标定表、flash 分区、构建目标与烧写 |

## 依赖

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Zephyr | v4.4.0 | 由 [west.yml](west.yml) 固定；RP2350 的 TRNG 驱动从 4.4 起在树内 |
| usp_zephyr / usp | v1.1.2-feature-202604 | Semtech 射频栈（RAC/RALF），射频驱动在这里；官方只有 feature tag |
| Zephyr SDK | 1.0.0 | 装 `arm-zephyr-eabi` 一个工具链即可 |
| 密码学 | Zephyr PSA Crypto | 设备侧 KDF 与每包 tag 走 PSA（provider 通常是 mbedTLS）；主机测试用一份等价的纯 C 实现 |

## 构建要点

1. **板子在本仓库里**：`zephyr/module.yml` 把仓库根同时作为 `board_root` 与 `dts_root`，所以不需要 `--shield`，也没有板级 overlay 要维护。
2. **换硬件只改 devicetree**：应用认的是 `lora-transceiver`、`zlrs-data-uart`、`zlrs-data-cdc`、`zlrs-crsf-cdc` / `zlrs-crsf-uart`、`zlrs-button`、`zlrs-led` 这些别名，引脚号不落到 C 代码里；别名缺失的端口自动裁掉（两条链路各一对 `-uart` / `-cdc`，两块都在时 USB 优先，见 `app/src/ports.c`）。
3. **工具链固定用 Zephyr SDK**：发行版那份 `arm-none-eabi-gcc` 的 newlib 没开 retargetable locking，会撞静态断言。
4. **镜像不许越过 1 MB**：链接区间取的是 flash 里的 code 分区，越界是编译错误，不会上板后才发现压掉了 storage。
5. **主机侧测试不碰工作区**：`tests/host` 只编译 `lib/` 里那同一份源码。

## 测试

`tests/host` 用假射频与假时钟跑仿真，不需要硬件：配对与会话、跳频锁步、RC/TLM 收发的防重放、400 字节跨帧透传与丢包恢复、失联与重连、同场多节点、畸形输入、时间戳与序号回绕，以及每个上板调试开关"关掉之后照常工作"都在里面。默认带 ASan + UBSan，开关见 [tests/host/Makefile](tests/host/Makefile)。

CI（[.github/workflows/build.yml](.github/workflows/build.yml)）跑同一套主机测试与各脚本的 self-test；固件构建单独一条流水线（[firmware.yml](.github/workflows/firmware.yml)），按 README 的流程搭工作区把 hf / lf 两个变体各构建一次。

射频与实时性相关的部分——回调延迟、换频开销、误包率、实际透传吞吐——只能在板上量，可以直接用 Semtech 官方示例，也可以用本仓库固件。当前进度与待办见 [doc/05-roadmap.md](doc/05-roadmap.md)。
