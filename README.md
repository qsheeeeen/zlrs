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
| [docs/01-system.md](docs/01-system.md) | 系统设计、开发阶段与待办 |
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

原型组合是 **XIAO nRF54L15 + Wio-LR2021**。工作区是 west 的 T2 拓扑：本
仓库自己就是 manifest 仓库，清单 [west.yml](west.yml) 里 pin 了 Zephyr
v4.2.0、usp_zephyr v1.0.0 和 usp v1.0.0。

```shell
tools/setup-zephyr.sh          # 默认建在仓库的上一级目录，首次约下载 5 GB
source tools/zephyr-env.sh
```

当前机器上的工作区是 `/home/qs/zlrs-worksapce`：

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

### 已知坑

1. **必须带 `--shield semtech_wio_lr2021`。** 不带就没有 LR2021 的 devicetree
   节点，usp 模块的源文件列表会是空的，CMake 在
   `usp_zephyr/modules/usp/CMakeLists.txt` 报错。

2. **应用要注册 `usp` 日志模块。** usp_zephyr 内部用
   `LOG_MODULE_DECLARE(usp, ...)`，但没有文件 `LOG_MODULE_REGISTER` 它；
   少了会在链接期报 `undefined reference to 'log_const_usp'`。
   本仓库用 [app/src/usp_log.c](app/src/usp_log.c) 处理。

3. **别混用系统 `arm-none-eabi-gcc`。** 发行版那份 newlib 没开
   retargetable locking，`CONFIG_NEWLIB_LIBC=y` 会撞上
   `static assertion failed: "Retargetable locking must be enabled"`。
   Zephyr SDK 的 newlib 没有这个问题——`tools/zephyr-env.sh` 一律把
   `ZEPHYR_TOOLCHAIN_VARIANT` 设成 `zephyr`，就是为了别走到那条路上去。

### 官方示例

验证工具链时可以直接编 Semtech 的示例（注意 `ROLE` 与两个 shield）：

```shell
cd "$ZLRS_WS"
west build -b xiao_nrf54l15/nrf54l15/cpuapp \
  --shield semtech_loraplus_expansion_board --shield semtech_wio_lr2021 \
  usp_zephyr/samples/usp/sdk/packet_error_rate_flrc \
  -- -DEXTRA_CFLAGS="-DROLE=RECEIVER"
```

## 测试

协议核心是纯 C、不依赖 Zephyr，可以直接在主机上编译运行：

```shell
make -C tests/host run
```

## 状态

设计已定案，代码尚未开始。当前进度见
[docs/01-system.md](docs/01-system.md) 第 9 节的开发阶段表。
