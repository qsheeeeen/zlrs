# zlrs

基于 Zephyr 的远距遥控 + 串口透传链路，射频芯片为 Semtech LR2021。

- 主诉求是 **UART 串口透传**。
- 不追求与官方 ExpressLRS 兼容；ELRS 仅作为协议结构的参考。
- 单颗 LR2021 分时双频：sub-GHz 跑控制链路，2.4GHz 跑透传数据链路。

文档：

| 文件 | 内容 |
| --- | --- |
| [docs/design.md](docs/design.md) | 总体设计与验证计划 |
| [docs/identity.md](docs/identity.md) | 身份、配对与密钥 |
| [docs/protocol.md](docs/protocol.md) | 空中格式与链路状态机 |
| [docs/threading.md](docs/threading.md) | 线程模型与 RAC 用法 |
| [docs/lr2021.md](docs/lr2021.md) | LR2021 参数摘要 |

仓库结构沿用 Zephyr
[example-application](https://github.com/zephyrproject-rtos/example-application)
的 workspace 应用（West T2）布局。

## 目录

| 路径 | 内容 |
| --- | --- |
| `app/` | Zephyr 应用入口（`west build -b <board> app`） |
| `drivers/` | 树外驱动，当前为 LR2021 射频 |
| `lib/` | 树外库，当前为协议核心 |
| `include/zlrs/` | 对外头文件 |
| `dts/bindings/` | 树外 devicetree 绑定 |
| `tests/` | twister 用例 + 主机侧测试 |
| `docs/` | 设计文档与器件参数摘要 |
| `boards/` | 自定义板（尚未添加） |

## 开始

需要一个可用的 Zephyr 开发环境，见官方 Getting Started Guide。

```shell
west init -m <zlrs-remote-url> --mr main zlrs-workspace
cd zlrs-workspace
west update
cd zlrs
west build -b <board> app
```

## 测试

协议核心是纯 C、不依赖 Zephyr 的，可以直接在主机上编译运行：

```shell
make -C tests/host run
```

Zephyr 侧的 ztest 通过 twister 运行：

```shell
west twister -T tests -p <platform>
```
