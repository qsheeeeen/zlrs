# 文档索引

zlrs 的文档按"从需求到实现、从设计到运维"的顺序编号。每份文档只拥有一类事实，别的地方只链接不复述；跨文档引用一律写文件名（不写"见上文"），代码与 Kconfig 里的 `doc/...` 引用以本表为准。

## 三条阅读路径

- **想把它用起来**：项目入口 [README](../README.md) → 硬件 [07-prototype](07-prototype.md) → 交互 [09-ux](09-ux.md)。
- **想改固件**：概览 [01-overview](01-overview.md) → 平台 [02-platform](02-platform.md) → 架构 [03-architecture](03-architecture.md) → 协议 [04-protocol](04-protocol.md)。
- **想了解设计取舍**：概览 [01-overview](01-overview.md) → 架构 [03-architecture](03-architecture.md) → 协议 [04-protocol](04-protocol.md) → 安全 [05-security](05-security.md)，再按需查芯片 [06-radio](06-radio.md) 与对比 [11-vs-elrs](11-vs-elrs.md)。

## 文档地图

| 文档 | 范围 | 不包含 |
| --- | --- | --- |
| [01-overview](01-overview.md) | 定位、服务对象、为什么自研、硬件取向、不做的事、性能目标、技术前提 | 具体实现与接口 |
| [02-platform](02-platform.md) | 硬件与软件基线、端口与工作模式、特性裁剪、上板调试开关、启动链与固件升级、错误处理约定 | 协议与时序细节 |
| [03-architecture](03-architecture.md) | 两条链路、频段角色、分时、时序预算与速率档、协议设计要点、软件分层、线程模型、诊断 | 帧格式（在 04） |
| [04-protocol](04-protocol.md) | 帧格式与包类型、链路状态机、会话握手、超帧、速率与功率自适应、跳频与法规 | 密钥与配对的安全论证（在 05） |
| [05-security](05-security.md) | 身份与密钥、配对与会话的安全性质、威胁模型、密码学参数、配置通道与配置协议帧 | 空中帧格式（在 04） |
| [06-radio](06-radio.md) | LR2021 的速率、灵敏度、切换时序、SPI/IRQ 与引脚摘要 | 我们怎么用它（在 03/04） |
| [07-prototype](07-prototype.md) | 现在的原型实物：两个模块、对外只有 USB、板内连线、模式 × 接口用法 | 载板设计与板级定义细节 |
| [08-carrier](08-carrier.md) | 载板 v1（草稿）：接插件、线序、电源与机械 | 原型现状 |
| [09-ux](09-ux.md) | 按键语义与状态灯的颜色/节奏 | 电路与驱动实现细节 |
| [10-roadmap](10-roadmap.md) | 里程碑、进度（已做 / 待验收 / 待做）与验收标准 | 设计论证 |
| [11-vs-elrs](11-vs-elrs.md) | 与 ExpressLRS 的事实对比、差距与差异化 | 本项目设计细节 |
| [板级 README](../boards/zlrs/rp2350_zero_core2021/README.md) | 板级定义：引脚与别名、两个 SKU 的标定表、flash 分区、构建目标与烧写 | 系统设计 |

## 约定

- 文档里的"本板"指 `boards/zlrs/rp2350_zero_core2021`，"原型"指现在桌上那套两模块接线（[07-prototype](07-prototype.md)）。
- 进度、待办与已知问题只在 [10-roadmap](10-roadmap.md) 记录；其它文档提到"待验收/待修"时链接过去。
- 引脚、分区数字、SKU 标定只在[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md) 定义；其它文档引用它。
