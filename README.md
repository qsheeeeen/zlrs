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

范围：本仓库包含协议核心、链路层与固件（射频链路、数据串口透传、配对持久化）。
串口侧的 CRSF 端口与配置协议不在其中；射频驱动来自 Semtech 的 USP，不在本仓库。

## 能编什么

| 产物 | 命令 | 说明 |
| --- | --- | --- |
| 固件（TX 或 RX） | `zlrs-build` | 目标 `xiao_nrf54l15/nrf54l15/cpuapp` + `semtech_wio_lr2021` shield |
| 主机侧测试 | `make -C tests/host run` | 协议核心与链路层的双节点仿真，不需要硬件 |
| 官方示例 | 见下 | Semtech 的 `packet_error_rate_flrc`，用来验证工具链与射频栈 |

```shell
# 官方示例：两个 shield 都要给，ROLE 决定收发方向
cd "$ZLRS_WS"
west build -b "$ZLRS_BOARD" \
  --shield semtech_loraplus_expansion_board --shield "$ZLRS_SHIELD" \
  usp_zephyr/samples/usp/sdk/packet_error_rate_flrc \
  -- -DEXTRA_CFLAGS="-DROLE=RECEIVER"
```

## 上手

### 1. 搭工作区

本仓库在 west 里是 manifest 仓库（T2 拓扑）：清单 [west.yml](west.yml) 固定了
Zephyr v4.2.0、usp_zephyr v1.0.0 与 usp v1.0.0，工作区默认建在仓库的上一级目录。

```shell
tools/setup-zephyr.sh        # 首次约 5 GB
source tools/zephyr-env.sh   # 每个新 shell 一次
```

工具链用 Zephyr SDK 0.17.4（`ZLRS_SDK_DIR` 可换）：脚本会检查，缺了会打印安装命令。

### 2. 构建与烧写

```shell
zlrs-build                   # = west build -b $ZLRS_BOARD --shield $ZLRS_SHIELD $ZLRS_APP
cd "$ZLRS_WS" && west flash  # 板级默认 pyocd，也可 --runner jlink/nrfutil/openocd
```

`tools/zephyr-env.sh` 导出这些变量，构建产物落在 `$ZLRS_WS/build/`：

| 变量 | 默认值 |
| --- | --- |
| `ZLRS_WS` | 仓库的上一级目录（工作区根） |
| `ZLRS_APP` | `$ZLRS_WS/zlrs/app` |
| `ZLRS_BOARD` | `xiao_nrf54l15/nrf54l15/cpuapp` |
| `ZLRS_SHIELD` | `semtech_wio_lr2021` |
| `ZLRS_SDK_DIR` | `~/zephyr-sdk-0.17.4` |

### 3. 角色、密钥与配对

- **角色**：默认 TX；RX 用 `-- -DCONFIG_ZLRS_APP_ROLE_TX=n` 关掉。
- **根密钥**：固件默认没有根密钥，上电后不参与收发。台架测试可以临时给一个：

  ```shell
  ZLRS_BENCH_KEY=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
  west build -b "$ZLRS_BOARD" --shield "$ZLRS_SHIELD" "$ZLRS_APP" \
    -- -DCONFIG_ZLRS_APP_ROOT_KEY_HEX=\"$ZLRS_BENCH_KEY\"
  ```

  正式流程是配置工具把根密钥写进 NVS，见 [docs/02-security.md](docs/02-security.md)。
- **配对**：开机后短按按键进入配对，长按解绑；设备标识与配对记录存在 NVS，
  重启后自动恢复。

## 代码结构

```
lib/proto/    协议核心（纯 C）：帧编解码、分片重组、跳频表、密钥派生与标签
lib/link/     链路层（纯 C）：状态机、配对与会话、超帧时隙、射频抽象接口
app/          Zephyr 应用：USP/RAC 移植、串口透传、NVS 持久化、超帧主循环
include/zlrs/ 对外头文件
tests/host/   主机侧测试与双节点仿真
tools/        工作区与环境的脚本
zephyr/       本仓库作为 Zephyr 模块的入口（module.yml）
docs/         设计文档
```

`lib/` 与 `include/` 不依赖 Zephyr（密码学原语用 mbedTLS）：协议核心与链路层在
主机上编译同一份源码，设备侧只多一个射频移植层。射频侧的一切（SPI 时序、校准、
频段、收发调度）都由 `usp_zephyr` 承担。

## 文档导引

| 想了解 | 看 |
| --- | --- |
| 需求与边界、硬件基线、两条链路与分时、软件分层、线程模型与回调契约 | [docs/01-system.md](docs/01-system.md) |
| 身份与密钥从哪来、配对流程、威胁模型与已知弱点 | [docs/02-security.md](docs/02-security.md) |
| 帧格式与包类型、链路状态机、超帧、跳频与法规约束 | [docs/03-protocol.md](docs/03-protocol.md) |
| LR2021 的速率、灵敏度、切换时序、SPI 与 IRQ 摘要 | [docs/04-lr2021.md](docs/04-lr2021.md) |

## 依赖

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Zephyr | v4.2.0 | 由 [west.yml](west.yml) 固定 |
| usp_zephyr / usp | v1.0.0 | Semtech 射频栈（RAC/RALF），射频驱动在这里 |
| Zephyr SDK | 0.17.4 | 装 `arm-zephyr-eabi` 一个工具链即可 |
| mbedTLS | Zephyr 自带 | 密码学原语；主机侧链接工作区里的同一份源码 |
| 参考硬件 | XIAO nRF54L15 + Wio-LR2021 | 换板子只需改 devicetree 与 Kconfig |

## 构建要点

1. **必须带 `--shield semtech_wio_lr2021`**：没有 LR2021 的 devicetree 节点时，
   usp 模块的源文件列表为空，CMake 会报错。
2. **应用侧要注册 `usp` 日志模块**（[app/src/usp_log.c](app/src/usp_log.c)）：
   usp_zephyr 内部只 `LOG_MODULE_DECLARE(usp, ...)`，没有对应的 REGISTER。
3. **工具链固定用 Zephyr SDK**：发行版那份 `arm-none-eabi-gcc` 的 newlib 没开
   retargetable locking，会撞静态断言。
4. **主机侧测试链接工作区里的 mbedTLS 源码**（与设备同一份），所以先跑过
   `tools/setup-zephyr.sh`；工作区不在默认位置时用 `MBEDTLS_DIR=` 指定。
5. **数据串口是 XIAO 的用户串口**（P2.07/P2.08，跨电源域）：固件按 usp_zephyr
   的已知问题说明打开了 CONSTLAT，否则没有调试器时 RX 收不到数据。

## 测试

`tests/host` 用假射频与假时钟跑两个节点的仿真：配对与会话、跳频锁步、RC/TLM
收发的防重放、400 字节跨帧透传与丢包恢复、失联与重连都在里面，不需要硬件。

射频与实时性相关的部分——回调延迟、换频开销、误包率、实际透传吞吐——只能在板上量，
可以直接用 Semtech 官方示例，也可以用本仓库固件；这一层的取舍见
[docs/01-system.md](docs/01-system.md)。
