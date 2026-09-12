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

原型组合是 **XIAO nRF54L15 + Wio-LR2021**。工作区脚本会搭出
Zephyr v4.2.0 + usp_zephyr v1.0.0 + usp：

```shell
tools/setup-zephyr.sh          # 默认建在 /tmp/zws，首次约下载 5 GB
source tools/zephyr-env.sh
```

之后编译本仓库的应用：

```shell
cd "$ZLRS_WS/ws"
west build -b "$ZLRS_BOARD" --shield semtech_wio_lr2021 "$ZLRS_REPO_ROOT/app"
```

不需要 Zephyr SDK——用系统自带的 `arm-none-eabi-gcc`。这也是唯一需要留意的
几处偏差（见下）。

### 已知坑

1. **不要用 newlib，用 picolibc。** Semtech 的示例 `prj.conf` 强制
   `CONFIG_NEWLIB_LIBC=y`，那是为 Zephyr SDK 准备的。发行版打包的
   `arm-none-eabi-gcc` 里那份 newlib 未开 retargetable locking，会撞上
   `static assertion failed: "Retargetable locking must be enabled"`。
   Zephyr 自带的 picolibc 没有这个问题，本仓库因此不指定 libc。

2. **必须带 `--shield semtech_wio_lr2021`。** 不带就没有 LR2021 的 devicetree
   节点，usp 模块的源文件列表会是空的，CMake 在
   `usp_zephyr/modules/usp/CMakeLists.txt` 报错。

3. **应用要注册 `usp` 日志模块。** usp_zephyr 内部用
   `LOG_MODULE_DECLARE(usp, ...)`，但没有文件 `LOG_MODULE_REGISTER` 它；
   少了会在链接期报 `undefined reference to 'log_const_usp'`。
   本仓库用 [app/src/usp_log.c](app/src/usp_log.c) 处理。

### 官方示例

验证工具链时可以直接编 Semtech 的示例（注意 `ROLE` 与两个 shield）：

```shell
cd "$ZLRS_WS/ws"
west build -b xiao_nrf54l15/nrf54l15/cpuapp \
  --shield semtech_loraplus_expansion_board --shield semtech_wio_lr2021 \
  usp_zephyr/samples/usp/sdk/packet_error_rate_flrc \
  -- -DEXTRA_CFLAGS="-DROLE=RECEIVER"
```

同样需要把 `CONFIG_NEWLIB_LIBC=y` 换成 picolibc，否则会撞上面第 1 条。

## 测试

协议核心是纯 C、不依赖 Zephyr，可以直接在主机上编译运行：

```shell
make -C tests/host run
```

## 状态

设计已定案，代码尚未开始。当前进度见
[docs/01-system.md](docs/01-system.md) 第 9 节的开发阶段表。
