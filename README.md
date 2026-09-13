# zlrs

基于 Zephyr、射频芯片为 Semtech LR2021 的点对点链路：一条 sub-GHz 控制链路
（遥控通道 + 遥测）加一条 2.4 GHz 数据链路（UART 串口透传）。TX 与 RX 是
同一份固件。

- **主诉求是串口透传**：两端的 UART 字节经空口互传；控制链路同时保证遥控可用，
  两条链路各跑一份相同的状态机，互不阻塞。
- **单颗射频分时双频**：默认 sub-GHz 跑控制、2.4 GHz 跑透传；频段与链路角色是
  配置项，竞速场景可以把两者对调。
- **一份固件适配多种硬件**：角色、频段、USB 之类的特性由 Kconfig 与 devicetree
  裁剪，不写 `#ifdef` 分支。
- **自研协议**：不与 ExpressLRS 兼容；控制距离目标 2 km，不做图传，不做产线密钥注入。

范围：本仓库包含协议核心、链路层、固件（射频链路、数据串口透传、配对持久化）
以及原型板的板级定义。串口侧的 CRSF 端口与配置协议还没做；射频驱动来自 Semtech
的 USP，不在本仓库。接下来做什么见 [doc/05-roadmap.md](doc/05-roadmap.md)。

## 硬件

原型是「微雪 RP2350-Zero（RP2350A，4 MB flash）+ 微雪 Core2021（LR2021 模块）」
这一套，板级定义在 [boards/zlrs/rp2350_zero_core2021](boards/zlrs/rp2350_zero_core2021/README.md)：
射频走 SPI0，BUSY/NRESET/DIO8 各一根 GPIO，透传口是 UART1，按键一根 GPIO，
4 MB flash 划成 1 MB code + 3 MB storage（NVS 存密钥与配对记录）。

Core2021 的 sub-G 是**两个 SKU**（PA 网络不同），所以板级是两个变体，代码同一份：

| 构建目标 | sub-G | 地区（`CONFIG_ZLRS_APP_REGION`） |
| --- | --- | --- |
| `rp2350_zero_core2021/rp2350a/m33/hf` | 850–930 MHz | FCC915(0) / EU868(1) |
| `rp2350_zero_core2021/rp2350a/m33/lf` | 410–510 MHz | CN470(2) / CN433(3) |

两个变体各带自己的 sub-G 标定表与标定频点；地区与本机 SKU 不匹配时固件**红灯
告警且不发射频**。2.4 GHz 那一路两个 SKU 相同——LR2021 是单颗分时双频，不需要
两颗模块。

## 能编什么

| 产物 | 命令 | 说明 |
| --- | --- | --- |
| 固件（TX 或 RX） | `zlrs-build` | 默认目标 `rp2350_zero_core2021/rp2350a/m33/hf`（LF SKU 用 `.../m33/lf`） |
| 主机侧测试 | `make -C tests/host run` | 协议核心、链路层（含 ARQ 与占空比）、CRSF 与配置协议，不需要硬件与工作区 |
| 官方示例 | 见下 | Semtech 的 `packet_error_rate_flrc`，用来单独验证工具链与射频栈 |

```shell
# 官方示例：直接跑在原型板上（板级定义里已经有 Core2021 的 devicetree 节点）
cd "$ZLRS_WS"
west build -b "$ZLRS_BOARD" usp_zephyr/samples/usp/sdk/packet_error_rate_flrc \
  -- -DEXTRA_CFLAGS="-DROLE=RECEIVER"
```

## 上手

### 1. 搭工作区

本仓库在 west 里是 manifest 仓库（T2 拓扑）：清单 [west.yml](west.yml) 固定
Zephyr v4.4.0、usp_zephyr 与 usp v1.1.2-feature-202604，工作区默认建在仓库的
上一级目录。

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

`scripts/zephyr-env.sh` 导出这些变量：

| 变量 | 默认值 |
| --- | --- |
| `ZLRS_WS` | 仓库的上一级目录（west 工作区根） |
| `ZLRS_APP` | 仓库里的 `app/` |
| `ZLRS_BOARD` | `rp2350_zero_core2021/rp2350a/m33` |
| `ZLRS_SDK_DIR` | `~/zephyr-sdk-1.0.0` |

### 3. 角色、密钥与配对

- **角色**：默认 TX；RX 用 `-- -DCONFIG_ZLRS_APP_ROLE_TX=n` 关掉。
- **根密钥**：固件默认没有根密钥，上电后不参与收发。正式做法是配置模式写入
  （见下）；台架测试也可以临时给一个：

  ```shell
  ZLRS_BENCH_KEY=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
  west build -b "$ZLRS_BOARD" "$ZLRS_APP" \
    -- -DCONFIG_ZLRS_APP_ROOT_KEY_HEX=\"$ZLRS_BENCH_KEY\"
  ```

  正式流程是配置工具把根密钥写进 NVS，见 [doc/02-security.md](doc/02-security.md)。
- **配对**：开机后长按按键（默认 1 秒）进入配对；设备标识与配对记录存在 NVS，
  重启后自动恢复。换绑也是长按重新配对，新记录顶掉旧的。

### 4. 配置模式

**开机时按住按键**进入配置模式：数据口（UART1）改跑配置协议，无操作 300 秒或
收到退出命令后回到正常收发。命令包括读状态、触发配对、写根密钥（十六进制
或短语，短语的派生固定在设备侧）、写失控值、改数据口波特率。帧格式与命令表见
[doc/02-security.md](doc/02-security.md)。

配置模式是个**物理动作**：不能远程进入，也不能在运行时透传的同时配置。

仓库自带一个最小配置工具（只用标准库，连设备时才需要 `pip install pyserial`）：

```shell
python3 scripts/zlrs-config.py --self-test                    # 不接设备，校验组帧
python3 scripts/zlrs-config.py --port /dev/ttyACM0 status
python3 scripts/zlrs-config.py --port /dev/ttyACM0 set-phrase "correct horse battery staple"
python3 scripts/zlrs-config.py --port /dev/ttyACM0 set-failsafe @default
python3 scripts/zlrs-config.py --port /dev/ttyACM0 exit
```

`--dry-run` 只打印要发的帧，方便对着抓包比对。

### 5. 地区与速率

- `CONFIG_ZLRS_APP_REGION`：0 = FCC915、1 = EU868（HF SKU），2 = CN470、
  3 = CN433（LF SKU）。地区决定控制链路的频段、发射功率上限、速率上限与占空比
  上限（窄频段放不下宽带 FLRC，会被压到 0.325 Mbps；EU868 是 1% 占空比，超出
  额度的时隙会被让出去并计数）。
- `CONFIG_ZLRS_APP_DATA_ARQ=y`：透传改走 ARQ（序号 + 累计 ACK + 重传），适合不能丢
  的数据；默认还是 best-effort，缓冲满就丢并计数。
- `CONFIG_ZLRS_APP_RATE_ADAPT=y`：按链路质量自动升降速率档。阈值要按实测曲线标定，
  所以默认关着，见 [doc/05-roadmap.md](doc/05-roadmap.md) 的 M4。
- `CONFIG_ZLRS_APP_DYNAMIC_POWER`（默认开）：按对端回报的 RSSI/LQ 自动升降
  发射功率，判据每秒评估一次，遥测断了回到上限；下限用
  `CONFIG_ZLRS_APP_DYNAMIC_POWER_FLOOR_DBM`。阈值按实测曲线标定。
- 窄带场景（433/868）以后允许换成 LoRa 等调制，见 [doc/05-roadmap.md](doc/05-roadmap.md)
  的 M4 待做项。

## 仓库结构

```
app/                  Zephyr 应用：USP/RAC 移植、串口透传、NVS 持久化、超帧主循环
boards/zlrs/          板级定义（原型板 RP2350-Zero + Core2021）
lib/proto/            协议核心（纯 C）：帧编解码、分片重组、跳频表、密钥派生与标签
                      遥测载荷
lib/link/             链路层（纯 C）：状态机、配对与会话、超帧时隙、射频抽象接口
                      ARQ profile、速率自适应策略
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

分层原则：`lib/` 与 `include/` 不依赖 Zephyr，协议核心与链路层在主机上编译同一份
源码，设备侧只多一个射频移植层（`app/src/rac_port.c`）。射频侧的一切——SPI 时序、
校准、频段、收发调度——都由 `usp_zephyr` 承担。

## 文档导引

| 想了解 | 看 |
| --- | --- |
| 需求与边界、硬件基线、两条链路与分时、软件分层、线程模型与回调契约 | [doc/01-system.md](doc/01-system.md) |
| 身份与密钥从哪来、配对流程、威胁模型与已知弱点 | [doc/02-security.md](doc/02-security.md) |
| 帧格式与包类型、链路状态机、超帧、跳频与法规约束 | [doc/03-protocol.md](doc/03-protocol.md) |
| LR2021 的速率、灵敏度、切换时序、SPI 与 IRQ 摘要 | [doc/04-lr2021.md](doc/04-lr2021.md) |
| 接下来的里程碑与各自的验收标准 | [doc/05-roadmap.md](doc/05-roadmap.md) |
| 按键语义与状态灯的颜色/节奏 | [doc/06-ui.md](doc/06-ui.md) |
| 载板 v1 的端口、网络与机械规格 | [doc/07-carrier.md](doc/07-carrier.md) |
| 原型板的接线与 Core2021 选型 | [boards/zlrs/rp2350_zero_core2021/README.md](boards/zlrs/rp2350_zero_core2021/README.md) |

## 依赖

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Zephyr | v4.4.0 | 由 [west.yml](west.yml) 固定；RP2350 的 TRNG 驱动从 4.4 起在树内 |
| usp_zephyr / usp | v1.1.2-feature-202604 | Semtech 射频栈（RAC/RALF），射频驱动在这里；官方只有 feature tag |
| Zephyr SDK | 1.0.0 | 装 `arm-zephyr-eabi` 一个工具链即可 |
| 密码学 | 本仓库自带 | 纯 C 的 SHA-256/HMAC/HKDF/PBKDF2，设备与主机同一份源码 |

## 构建要点

1. **板子在本仓库里**：`zephyr/module.yml` 把仓库根同时作为 `board_root` 与
   `dts_root`，所以不需要 `--shield`，也没有板级 overlay 要维护。
2. **换硬件只改 devicetree**：应用认的是 `lora-transceiver`、`zlrs-data-uart`、
   `zlrs-crsf-uart`、`zlrs-button` 这些别名，引脚号不落到 C 代码里（CRSF 端口
   在原型板上是 USB CDC ACM，自定义板指到 UART 即可）。
3. **工具链固定用 Zephyr SDK**：发行版那份 `arm-none-eabi-gcc` 的 newlib 没开
   retargetable locking，会撞静态断言。
4. **镜像不许越过 1 MB**：链接区间取的是 flash 里的 code 分区，越界是编译错误，
   不会上板后才发现压掉了 storage。
5. **主机侧测试不碰工作区**：`tests/host` 只编译 `lib/` 里那同一份源码。

## 测试

`tests/host` 用假射频与假时钟跑两个节点的仿真：配对与会话、跳频锁步、RC/TLM
收发的防重放、400 字节跨帧透传与丢包恢复、失联与重连都在里面，不需要硬件。

CI（[.github/workflows/build.yml](.github/workflows/build.yml)）跑同一套 host
测试，另外按 README 的流程搭工作区构建一次原型板固件。

射频与实时性相关的部分——回调延迟、换频开销、误包率、实际透传吞吐——只能在板上量，
可以直接用 Semtech 官方示例，也可以用本仓库固件；这一层的取舍见
[doc/01-system.md](doc/01-system.md)。
