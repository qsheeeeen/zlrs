# 文档索引

文档分成四块：**产品设计**（需求：要做什么、做到什么程度）、**硬件**、**软件**（这两块是实现）、**开发**（调试与计划）。编号即阅读顺序；每份文档只拥有一类事实，别处只链接。

写作约定：

- 只描述**最终形态**。没定下来的地方留白，不写"待确认 / 草稿 / TBD"；过程与进度一律进 [13-roadmap](13-roadmap.md)。
- 面向使用者：调试相关的内容集中在 [12-debug](12-debug.md)，其它文档不出现。
- 引脚、别名、分区数字只在[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md) 定义，别处引用。

## 一、产品设计（需求）

| 文档 | 范围 |
| --- | --- |
| [01-product](01-product.md) | 定位、三种链路形态与场景、距离 × 速率目标、性能指标、边界 |
| [02-vs-elrs](02-vs-elrs.md) | 与 ExpressLRS 的关系：参考了什么、对立在哪 |

## 二、硬件（实现）

| 文档 | 范围 |
| --- | --- |
| [03-platform](03-platform.md) | 目标硬件形态、板级、构建目标、工具链、分区与烧录 |
| [04-prototype](04-prototype.md) | 现在的原型：两个模块 + USB，按键用杜邦线模拟 |
| [05-carrier](05-carrier.md) | 载板：USB + 按键 + 两路 6P 串口 + 电源 |
| [06-radio](06-radio.md) | LR2021 参数摘要 |

## 三、软件（实现）

| 文档 | 范围 |
| --- | --- |
| [07-architecture](07-architecture.md) | 两条链路、频段角色、分时、时序预算、分层、线程 |
| [08-firmware](08-firmware.md) | 端口与端点、工作模式、运行期配置、裁剪、错误处理约定 |
| [09-protocol](09-protocol.md) | 帧格式、链路状态机、会话、同步、可靠传输、跳频与法规 |
| [10-security](10-security.md) | 身份与密钥、配对、威胁模型、配置协议 |
| [11-ux](11-ux.md) | 按键语义与状态灯 |

## 四、开发

| 文档 | 范围 |
| --- | --- |
| [12-debug](12-debug.md) | debug overlay、分层调试开关、上板调试方法 |
| [13-roadmap](13-roadmap.md) | 待办与验收标准 |

## 阅读路径

- 想把它用起来：[README](../README.md) → [04-prototype](04-prototype.md) → [11-ux](11-ux.md)
- 想改固件：[01-product](01-product.md) → [07-architecture](07-architecture.md) → [08-firmware](08-firmware.md) → [09-protocol](09-protocol.md)
- 想了解设计取舍：[01-product](01-product.md) → [02-vs-elrs](02-vs-elrs.md) → [07-architecture](07-architecture.md) → [09-protocol](09-protocol.md) → [10-security](10-security.md)
