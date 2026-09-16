# zlrs

基于 Zephyr、射频芯片为 Semtech LR2021 的点对点链路：一条 sub-GHz 控制链路（遥控通道 + 遥测）加一条 2.4 GHz 数据链路（UART 串口透传）。TX 与 RX 是同一份固件。

- **主诉求是串口透传**：两端的 UART 字节经空口互传；控制链路同时保证遥控可用，两条链路各跑一份相同的状态机，互不阻塞。
- **单颗射频分时双频**：默认 sub-GHz 跑控制、2.4 GHz 跑透传；频段与链路角色是配置项，竞速场景可以把两者对调。
- **一份固件适配多种硬件**：角色、频段、USB 之类的特性由 Kconfig 与 devicetree 裁剪，不写 `#ifdef` 分支。
- **自研协议**：不与 ExpressLRS 兼容；控制距离目标 2 km，不做图传，不做产线密钥注入。

范围：本仓库包含协议核心、链路层、固件（射频链路、CRSF 端口、串口透传、配置模式、配对持久化）以及板级定义；射频驱动来自 Semtech 的 USP，不在本仓库。接下来做什么见 [doc/05-roadmap.md](doc/05-roadmap.md)。

## 硬件

硬件是「微雪 RP2350-Zero（RP2350A，4 MB flash）+ 微雪 Core2021（LR2021 模块）」这一套，板级定义在 [boards/zlrs/rp2350_zero_core2021](boards/zlrs/rp2350_zero_core2021/README.md)：射频走 SPI0，BUSY/NRESET/DIO8 各一根 GPIO，CRSF 与数据口各占一路 UART（GP0/1、GP8/9）并在 USB 上各有一块 CDC，控制台在 USB 第三个 CDC；按键一根 GPIO，4 MB flash 划成 1 MB code + 3 MB storage（NVS 存密钥与配对记录）。

Core2021 的 sub-G 是**两个 SKU**（PA 网络不同），所以板级是两个变体，代码同一份：

| 构建目标 | sub-G | 地区（`CONFIG_ZLRS_APP_REGION`） |
| --- | --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | 850–930 MHz | FCC915(0) / EU868(1) |
| `rp2350_zero_core2021/rp2350a/m33/lf` | 410–510 MHz | CN470(2) / CN433(3) |

两个变体各带自己的 sub-G 标定表与标定频点；地区与本机 SKU 不匹配时固件**红灯告警且不发射频**。2.4 GHz 那一路两个 SKU 相同——LR2021 是单颗分时双频，不需要两颗模块。

## 能编什么

| 产物 | 命令 | 说明 |
| --- | --- | --- |
| 固件（TX 或 RX） | `zlrs-build` | 默认目标 `rp2350_zero_core2021/rp2350a/m33/hf`（LF SKU 用 `.../m33/lf`） |
| 主机侧测试 | `make -C tests/host run` | 协议核心、链路层（含 ARQ 与占空比）、CRSF 与配置协议，不需要硬件与工作区 |
| 官方示例 | 见下 | Semtech 的 `packet_error_rate_flrc`，用来单独验证工具链与射频栈 |

```shell
# 官方示例：直接跑在这块板上（板级定义里已经有 Core2021 的 devicetree 节点）
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

### 2. 构建与烧写

```shell
zlrs-build                     # = west build -b $ZLRS_BOARD $ZLRS_APP
cd "$ZLRS_WS" && west flash    # 默认 openocd，也可 --runner uf2/probe-rs/jlink
```

按住 BOOTSEL 上电后拖 `zephyr.uf2` 也可以，产物在 `$ZLRS_WS/build/zephyr/`。

kernel log 与 shell 在 USB 的第三个 CDC 上（Linux 下 `/dev/ttyACM2`）。

`scripts/zephyr-env.sh` 导出这些变量：

| 变量 | 默认值 |
| --- | --- |
| `ZLRS_WS` | 仓库的上一级目录（west 工作区根） |
| `ZLRS_APP` | 仓库里的 `app/` |
| `ZLRS_BOARD` | `rp2350_zero_core2021/rp2350a/m33/hf`（LF SKU 用 `.../lf`） |
| `ZLRS_SDK_DIR` | `~/zephyr-sdk-1.0.0` |

### 3. 角色、密钥与配对

- **角色**：默认 TX；RX 用 `-- -DCONFIG_ZLRS_APP_ROLE_TX=n` 关掉。
- **根密钥**：固件默认没有根密钥，上电后不参与收发。正式做法是配置模式写入（见下）；台架测试也可以临时给一个：

  ```shell
  ZLRS_BENCH_KEY=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
  west build -b "$ZLRS_BOARD" "$ZLRS_APP" \
    -- -DCONFIG_ZLRS_APP_ROOT_KEY_HEX=\"$ZLRS_BENCH_KEY\"
  ```

正式流程是配置工具把根密钥写进 NVS，见 [doc/02-security.md](doc/02-security.md)。
- **配对**：开机后长按按键（默认 1 秒）进入配对；配对记录存在 NVS，重启后两条链路都自动恢复，不必重新配对。换绑也是长按重新配对，新记录顶掉旧的。

### 4. 配置模式

**开机时按住按键**进入配置模式：数据口改跑配置协议，无操作 300 秒或收到退出命令后回到正常收发。数据口有两块端点（UART1 与 USB 数据 CDC），主机把它打开就走 USB（见 [doc/01-system.md](doc/01-system.md) 2.1）。命令包括读状态、触发配对、写根密钥、写失控值、改数据口波特率；帧格式见 [doc/02-security.md](doc/02-security.md)。

配置模式是个**物理动作**：不能远程进入，也不能在运行时透传的同时配置。

仓库自带一个最小配置工具（只用标准库，连设备时才需要 `pip install pyserial`）：

USB 三个 CDC 依次是 CRSF、数据口、控制台（`/dev/ttyACM0/1/2`）；不插 USB 时数据口走 UART1（USB 串口线一般是 `/dev/ttyUSB0`）。

```shell
python3 scripts/zlrs-config.py --self-test                    # 不接设备，校验组帧
python3 scripts/zlrs-config.py --port /dev/ttyACM1 status
python3 scripts/zlrs-config.py --port /dev/ttyACM1 set-phrase "correct horse battery staple"
python3 scripts/zlrs-config.py --port /dev/ttyACM1 set-failsafe @default
python3 scripts/zlrs-config.py --port /dev/ttyACM1 exit
```

`--dry-run` 只打印要发的帧，方便对着抓包比对。

### 5. 地区与速率

- `CONFIG_ZLRS_APP_REGION`：0 = FCC915、1 = EU868（HF SKU），2 = CN470、3 = CN433（LF SKU）。地区决定控制链路的频段、发射功率上限、速率上限与占空比上限（窄频段放不下宽带 FLRC，会被压到 0.325 Mbps；EU868 是 1% 占空比，超出额度的时隙会被让出去并计数）。
- `CONFIG_ZLRS_APP_DATA_ARQ=y`：透传改走 ARQ（序号 + 累计 ACK + 重传），适合不能丢的数据；默认还是 best-effort，缓冲满就丢并计数。
- `CONFIG_ZLRS_APP_CONTROL_RATE_HZ`（默认 150，范围 50–500）：控制帧率档。实际生效值会被地区占空比与空中时间压档——EU868 的 1% 只撑得住 ~46 Hz，窄带速率下上限 ~470 Hz；500 Hz 只在不限占空比的地区有意义。见 [doc/03-protocol.md](doc/03-protocol.md) 4.1。
- `CONFIG_ZLRS_APP_RATE_ADAPT=y`：按链路质量自动升降速率档。阈值要按实测曲线标定，所以默认关着，见 [doc/05-roadmap.md](doc/05-roadmap.md) 的 M4。
- `CONFIG_ZLRS_APP_DYNAMIC_POWER`（默认开）：按对端回报的 RSSI/LQ 自动升降发射功率，判据每秒评估一次，遥测断了回到上限；下限用 `CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM`。阈值按实测曲线标定。
- `CONFIG_ZLRS_APP_RESYNC_PERIOD_MS`（默认 1000，0 = 关）：连接态周期 `BEACON`。RX 据此把跳频索引拉回来（两端晶振差会累积成"踩空"），同一条路也让对端重启后能自动重连，见 [doc/03-protocol.md](doc/03-protocol.md) 5.1。
- `CONFIG_ZLRS_APP_CONTROL_MODULATION_LORA=y`：控制链路改跑 LoRa 远距档（默认 SF7 / BW125 / CR 4/5 / 8 符号前导），数据链路仍留在 2.4G 跑 FLRC。超帧周期由最慢的那条链路决定——SF7/BW125 下 31 B 的 RC 帧要 72 ms，所以配置的 10 Hz 会被空中时间压到 9 Hz；EU868 的 1% 占空比撑不住这一档（压到 1 Hz 并告警）。参数与换算见 [doc/03-protocol.md](doc/03-protocol.md) 4.1.1。

### 6. 上板调试的开关

硬件第一次上电时不要一次把所有东西都打开：每一项复杂功能都有自己的 Kconfig 开关，从最简形态往上逐层验证。关掉的项开机会打一条 `bring-up:` 警告，整套思路与推荐顺序见 [doc/01-system.md](doc/01-system.md) 2.3。

| 开关 | 关掉之后 |
| --- | --- |
| `CONFIG_ZLRS_APP_SECURITY_PROFILE_P0=y` | 帧不带 tag，先确认收发时序与帧格式 |
| `CONFIG_ZLRS_APP_FHSS=n` | 固定发在同步信道，不跳频 |
| `CONFIG_ZLRS_APP_SESSION_HANDSHAKE=n` | 配对级钥匙直接当会话钥匙：BEACON 之后只回一条 `SESS_RESP` |
| `CONFIG_ZLRS_APP_TELEMETRY=n` | 控制链路只剩下行 RC（TX 的失联判定一并关掉） |
| `CONFIG_ZLRS_APP_DATA_ARQ=n` / `ZLRS_APP_RATE_ADAPT=n` / `ZLRS_APP_DYNAMIC_POWER=n` | 透传 best-effort / 固定速率档 / 固定发射功率 |
| `CONFIG_ZLRS_APP_RESYNC_PERIOD_MS=0` | 不做连接态周期 `BEACON`（跳频只在对齐时校准，对端重启不自动重连） |

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
lib/proto/            协议核心：帧编解码、分片重组、跳频表、密钥派生与标签
                      （原语走 PSA，见 lib/proto/crypto_psa.c）
                      遥测载荷、调制档与空中时间模型（FLRC / LoRa）
lib/link/             链路层（纯 C）：状态机、配对与会话、超帧时隙、射频抽象接口
                      ARQ profile、速率自适应策略（档位 = 调制 + 参数）
lib/crsf/             CRSF 端口编解码（纯 C）：RC 通道帧与链路统计帧
lib/config/           配置协议（纯 C）：帧格式 + 命令分发（落盘通过回调）
lib/region/           地区配置层（纯 C）：信道表、功率上限、占空比记账
lib/led/              状态灯模式引擎（纯 C）：状态 → 颜色与节奏
include/zlrs/         对外头文件
tests/host/           主机侧测试与双节点仿真
scripts/              工作区与环境的脚本
doc/                  设计文档
zephyr/module.yml     本仓库作为 Zephyr 模块的入口（板级定义也挂在这里）
```

分层原则：`lib/` 与 `include/` 不依赖 Zephyr，协议核心与链路层在主机上编译同一份源码，设备侧只多一个射频移植层（`app/src/rac_port.c`）。射频侧的一切——SPI 时序、校准、频段、收发调度——都由 `usp_zephyr` 承担。

## 文档导引

| 想了解 | 看 |
| --- | --- |
| 需求与边界、硬件基线、两条链路与分时、软件分层、线程模型与回调契约 | [doc/01-system.md](doc/01-system.md) |
| 身份与密钥从哪来、配对流程、威胁模型与已知弱点 | [doc/02-security.md](doc/02-security.md) |
| 帧格式与包类型、链路状态机、超帧、跳频与法规约束 | [doc/03-protocol.md](doc/03-protocol.md) |
| LR2021 的速率、灵敏度、切换时序、SPI 与 IRQ 摘要 | [doc/04-lr2021.md](doc/04-lr2021.md) |
| 接下来的里程碑与各自的验收标准 | [doc/05-roadmap.md](doc/05-roadmap.md) |
| 按键语义与状态灯的颜色/节奏 | [doc/06-ui.md](doc/06-ui.md) |
| 载板的接线、接插口与机械规格 | [doc/07-carrier.md](doc/07-carrier.md) |
| 接线与 Core2021 选型 | [boards/zlrs/rp2350_zero_core2021/README.md](boards/zlrs/rp2350_zero_core2021/README.md) |
| 与 ExpressLRS 的功能对比（差距与差异化） | [doc/08-vs-elrs.md](doc/08-vs-elrs.md) |

## 依赖

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Zephyr | v4.4.0 | 由 [west.yml](west.yml) 固定；RP2350 的 TRNG 驱动从 4.4 起在树内 |
| usp_zephyr / usp | v1.1.2-feature-202604 | Semtech 射频栈（RAC/RALF），射频驱动在这里；官方只有 feature tag |
| Zephyr SDK | 1.0.0 | 装 `arm-zephyr-eabi` 一个工具链即可 |
| 密码学 | Zephyr PSA Crypto | 设备侧 KDF 与每包 tag 走 PSA（provider 通常是 mbedTLS）；主机测试用一份等价的纯 C 实现 |

## 构建要点

1. **板子在本仓库里**：`zephyr/module.yml` 把仓库根同时作为 `board_root` 与 `dts_root`，所以不需要 `--shield`，也没有板级 overlay 要维护。
2. **换硬件只改 devicetree**：应用认的是 `lora-transceiver`、`zlrs-data-uart`、`zlrs-data-cdc`、`zlrs-crsf-cdc` / `zlrs-crsf-uart`、`zlrs-button` 这些别名，引脚号不落到 C 代码里；别名缺失的端口自动裁掉（两条链路各一对 `-uart` / `-cdc`，两块都在时 USB 优先，见 app/src/ports.c）。
3. **工具链固定用 Zephyr SDK**：发行版那份 `arm-none-eabi-gcc` 的 newlib 没开 retargetable locking，会撞静态断言。
4. **镜像不许越过 1 MB**：链接区间取的是 flash 里的 code 分区，越界是编译错误，不会上板后才发现压掉了 storage。
5. **主机侧测试不碰工作区**：`tests/host` 只编译 `lib/` 里那同一份源码。

## 测试

`tests/host` 用假射频与假时钟跑仿真，不需要硬件：配对与会话、跳频锁步、RC/TLM 收发的防重放、400 字节跨帧透传与丢包恢复、失联与重连、同场多节点、畸形输入、时间戳与序号回绕，以及每个上板调试开关"关掉之后照常工作"都在里面。默认带 ASan + UBSan，开关见 [tests/host/Makefile](tests/host/Makefile)。

CI（[.github/workflows/build.yml](.github/workflows/build.yml)）跑同一套 host 测试，另外按 README 的流程搭工作区构建一次固件。

射频与实时性相关的部分——回调延迟、换频开销、误包率、实际透传吞吐——只能在板上量，可以直接用 Semtech 官方示例，也可以用本仓库固件；这一层的取舍见 [doc/01-system.md](doc/01-system.md)。
