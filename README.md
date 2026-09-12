# zlrs

基于 Zephyr 的远距遥控 + 串口透传链路，射频芯片为 Semtech LR2021。

- 主诉求是 **UART 串口透传**。
- 不追求与官方 ExpressLRS 兼容；ELRS 仅作为协议结构的参考。
- 单颗 LR2021 分时双频：默认 sub-GHz 跑控制链路、2.4GHz 跑透传；
  频段角色可配。
- 目标：一套方案覆盖固定翼、多旋翼、车、船。

## 文档

| 文件 | 内容 |
| --- | --- |
| [docs/01-system.md](docs/01-system.md) | 系统设计：链路、硬件基线、分时与软件分层 |
| [docs/02-security.md](docs/02-security.md) | 安全模型：身份、配对与密钥 |
| [docs/03-protocol.md](docs/03-protocol.md) | 空中格式、超帧与链路状态机 |
| [docs/04-threading.md](docs/04-threading.md) | 线程模型与 RAC 用法 |
| [docs/05-lr2021.md](docs/05-lr2021.md) | LR2021 参数摘要 |

## 目录

| 路径 | 内容 |
| --- | --- |
| `app/` | Zephyr 应用入口 |
| `lib/proto/` | 纯 C、无 Zephyr 依赖的协议核心 |
| `include/zlrs/` | 对外头文件 |
| `tools/` | 构建环境脚本 |
| `tests/host/` | 主机侧单元测试 |
| `docs/` | 设计文档 |

射频驱动不在这里——由 Semtech 官方的
[usp_zephyr](https://github.com/Lora-net/usp_zephyr) 提供。

## 构建环境

参考硬件组合是 **XIAO nRF54L15 + Wio-LR2021**。工作区是 west 的 T2 拓扑：本
仓库自己就是 manifest 仓库，清单 [west.yml](west.yml) 里 pin 了 Zephyr
v4.2.0、usp_zephyr v1.0.0 和 usp v1.0.0。

```shell
tools/setup-zephyr.sh          # 默认建在仓库的上一级目录，首次约下载 5 GB
source tools/zephyr-env.sh
```

本机的工作区在 `/home/qs/zlrs-worksapce`：

| 路径 | 内容 |
| --- | --- |
| `zlrs-worksapce/zlrs/` | 本仓库，也就是 manifest 仓库（`west.yml` 的 `self.path`） |
| `zlrs-worksapce/zephyr/` | Zephyr v4.2.0，外加 import 进来的一堆 module |
| `zlrs-worksapce/usp_zephyr/`、`zlrs-worksapce/modules/lib/usp/` | Semtech 的 USP 栈 |
| `zlrs-worksapce/.venv/` | west 与构建依赖（Python venv） |
| `zlrs-worksapce/build/` | `west build` 的输出目录 |

工具链用 **Zephyr SDK 0.17.4**（默认 `~/zephyr-sdk-0.17.4`，设
`ZLRS_SDK_DIR` 可换）。SDK 只装 `arm-zephyr-eabi` 一个工具链就够：

```shell
cd ~/zephyr-sdk-0.17.4 && ./setup.sh -t arm-zephyr-eabi -h -c
```

`setup-zephyr.sh` 会检查这一步，缺工具链时直接把命令打出来。环境就绪后编译
本仓库的应用：

```shell
zlrs-build      # = cd $ZLRS_WS && west build -b $ZLRS_BOARD --shield $ZLRS_SHIELD $ZLRS_APP
```

等价的手写形式：

```shell
cd "$ZLRS_WS"
west build -b "$ZLRS_BOARD" --shield "$ZLRS_SHIELD" "$ZLRS_APP"
```

### 构建要点

1. **构建必须带 `--shield semtech_wio_lr2021`。** 没有 LR2021 的 devicetree
   节点时，usp 模块的源文件列表为空，CMake 在
   `usp_zephyr/modules/usp/CMakeLists.txt` 报错。

2. **应用需要注册 `usp` 日志模块。** usp_zephyr 内部只用
   `LOG_MODULE_DECLARE(usp, ...)`，注册放在
   [app/src/usp_log.c](app/src/usp_log.c)。

3. **工具链固定用 Zephyr SDK。** `tools/zephyr-env.sh` 把
   `ZEPHYR_TOOLCHAIN_VARIANT` 设为 `zephyr`。发行版的 `arm-none-eabi-gcc`
   不能替代：它的 newlib 没开 retargetable locking，会撞上
   `static assertion failed: "Retargetable locking must be enabled"`。

### 官方示例

Semtech 的示例可以直接编（注意 `ROLE` 与两个 shield）：

```shell
cd "$ZLRS_WS"
west build -b xiao_nrf54l15/nrf54l15/cpuapp \
  --shield semtech_loraplus_expansion_board --shield semtech_wio_lr2021 \
  usp_zephyr/samples/usp/sdk/packet_error_rate_flrc \
  -- -DEXTRA_CFLAGS="-DROLE=RECEIVER"
```

## 测试

协议核心与链路层可以直接在主机上编译运行。密码学原语链接的是工作区里的
mbedTLS 源码——跟设备侧同一份，所以先跑过 `tools/setup-zephyr.sh`：

```shell
make -C tests/host run
```
