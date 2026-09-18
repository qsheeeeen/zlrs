# 原型：RP2350-Zero + Core2021

状态：**桌上原型**——RP2350-Zero 与 Core2021 两块模块用线连起来。对外的接口**只有 USB-C**（供电、三块 CDC、烧写）；CRSF 与数据是模块焊盘上的两路 UART，原型上没接外设，要用就自己引线。

本文写这套实物的接口与用法：有哪些口、软件在各种模式下怎么用它们、怎么烧写。引脚与别名的完整定义见 [板级 README](../boards/zlrs/rp2350_zero_core2021/README.md)，模式的设计理由见 [01-system.md](01-system.md) 2.1/2.4。

## 1. 实物构成

| 部件 | 规格 | 备注 |
| --- | --- | --- |
| 主控 | 微雪 RP2350-Zero：RP2350A（Cortex-M33）、4 MB QSPI flash、原生 USB-C、板载 WS2812B | BOOTSEL 与 RESET 在模块上；不引 SWD |
| 射频 | 微雪 Core2021（Semtech LR2021） | 单颗分时双频：sub-G + 2.4G |
| SKU | **LF**（410–510 MHz） | 可用地区 CN470(2) / CN433(3)；与 SKU 不匹配时红灯告警且不发射频 |
| 天线 | 433 MHz，接模块天线座 | 2.4G 那一路的实走线按模块图纸核对 |
| 供电 | USB-C | Core2021 的 3V3 由 Zero 供，两块模块共地 |
| 模块时钟 | 无源晶振，不是 TCXO | 所以 DT 里 `tcxo-*` 保持关断，`reg-mode` 用 DC/DC |
| 现状 | Core2021 还没接上 | 跑 `CONFIG_ZLRS_APP_RADIO=n` 的 bring-up 档：端口、配置模式、升级链已验，射频未验 |

## 2. 物理接口

### 2.1 对外：USB-C

只有一个口，按运行模式以三种面孔出现：

| 模式 | USB 上是什么 |
| --- | --- |
| 应用 | 3 块 CDC：CRSF / 数据 / 控制台 |
| MCUboot 恢复 | 2 块 CDC：恢复 + 控制台（CRSF 那块被裁掉） |
| ROM BOOTSEL | `RPI-RP2` 大容量盘（拖 UF2），没有 CDC |

两种 USB 模式自报的名字不同：应用是 `ZLRS`、bootloader 是 `ZLRS MCUBOOT`。主机脚本按 `/dev/serial/by-id/...` 找口，不写死 `ttyACM*` 编号——应用模式下三块 CDC 依次是 CRSF / 数据 / 控制台。

### 2.2 板内连线（Zero ↔ Core2021）

只在 `CONFIG_ZLRS_APP_RADIO=y` 时使用；`=n` 时整块射频不碰（含 SPI 与三根控制线），见 `app/src/radio_null.c`。

| 信号 | RP2350-Zero | Core2021 | 说明 |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | 对应脚 | 总线 16 MHz |
| SPI0 CS | GP5 | 对应脚 | 普通 GPIO 片选（`cs-gpios`），不用硬件 CSN |
| BUSY | GP20 | BUSY | 高有效，内部上拉 |
| NRESET | GP21 | NRESET | 低有效 |
| DIO8（IRQ） | GP22 | DIO8 | 高有效，内部下拉 |
| 3V3 / GND | 3V3、GND | 3V3、GND | 由 Zero 供 |

没接的东西：CRSF 在 GP0/GP1（UART0，420000）、数据口在 GP8/GP9（UART1，默认 115200）、按键在 GP12——都是模块焊盘，原型上悬空。控制台不走引脚，在 USB 第三块 CDC 上。

### 2.3 板上的器件

| 器件 | 位置 | 用途 |
| --- | --- | --- |
| BOOTSEL 按键 | RP2350-Zero | 芯片级：按住插 USB 进 ROM 的 UF2 模式 |
| RESET 按键 | RP2350-Zero | 复位；复位后 300 ms 内收到合法 mcumgr 帧会停在恢复模式 |
| WS2812B | RP2350-Zero（GP16） | 状态灯，语义见 [07-ui.md](07-ui.md) |
| 天线座 | Core2021 | 接 433 MHz 天线 |

## 3. 软件模式 × 接口用法

| 接口 | 应用模式 | 配置模式 | MCUboot 恢复 | ROM BOOTSEL |
| --- | --- | --- | --- | --- |
| USB-C | 3 块 CDC：CRSF / 数据 / 控制台 | 同左；控制台可敲 shell | 2 块 CDC：恢复 + 控制台 | `RPI-RP2` 大容量盘 |
| UART0 GP0/1 | CRSF 收发 420000 | 端口映射不变，但超帧停，不转发 | 不初始化 | 不跑固件 |
| UART1 GP8/9 | 字节透传 | 改跑配置协议 | **不用**（恢复走 USB） | 不跑固件 |
| 射频 SPI / BUSY / DIO8 | 每帧四个时隙收发 | 不发事务 | 不碰 | 不跑固件 |
| 按键 GP12 | 运行中长按 ≥1 s 配对 | 会话里不看按键 | 刻意不用（`BOOT_SERIAL_ENTRANCE_GPIO=n`） | BOOTSEL 是另一颗芯片级按钮 |
| 状态灯 | 颜色 + 节奏见 [07-ui.md](07-ui.md) | 白灯 0.5 Hz | MCUboot 不驱动，保持复位前最后一帧颜色 | 不驱动 |
| 看门狗 wdt0 | 默认关；开了主循环喂 | 会话期间 `disarm`，退出再 `arm` | `lib/bootguard` 先清硬件 `ENABLE` 位 | — |

### 3.1 应用模式（正常收发）

两条逻辑链路（CRSF、数据），每条都有两块端点二选一：主机的 CDC 被打开（DTR 有效）**或**该端点收到过主机字节，就走 USB；否则走 UART；非当前端点的进账直接丢。判据的由来与 ISR 时序见 [01-system.md](01-system.md) 2.1，实现在 `app/src/ports.c` 的 `port_active()`。

控制台固定钉在 USB 第三块 CDC（`zephyr,console` = `cdc_acm_uart2`），不参与二选一；数据口上的控制魔数是 `zlrs!configmode!`（进配置）与 `zlrs!bootloader!`（进恢复），只有匹配到前缀的字节被扣住，正常数据不丢不重（`app/src/main.c`）。

### 3.2 配置模式

数据口切换成配置协议（读状态、触发配对、写根密钥、写失控值、改数据口波特率），`EXIT` 或空闲超时（默认 300 s）退出；写进去的密钥与失控值当场生效。三个入口、各自一个 Kconfig：

| 入口 | 开关 | 默认 | 本原型能不能用 |
| --- | --- | --- | --- |
| 开机按住按键 | `CONFIG_ZLRS_APP_CONFIG_MODE` | 开 | 不能：GP12 没接 |
| 数据口魔数 | `CONFIG_ZLRS_APP_CONFIG_MAGIC` | 关 | 能，上板验证走的就是这条 |
| 控制台 shell `zlrs config` | `CONFIG_ZLRS_APP_SHELL` | 关 | 能（`zlrs status` / `ports` / `recovery` 同在此开关下） |

三个入口进的是同一个会话，而且都在主循环里跑（shell 只置一个请求）：会话期间超帧暂停、数据口不会被透传抢字节；会话开始先 `zlrs_watchdog_disarm()`（写短语要几十秒 PBKDF2，会被 8 s 看门狗复位），退出再武装。

### 3.3 MCUboot 串口恢复

只走 USB。bootloader 模式下 USB 只剩两块 CDC：第 1 块（= 应用的数据口那块 `cdc_acm_uart1`，`zephyr,uart-mcumgr`）收 mcumgr 帧，第 2 块是内核日志；CRSF 那块由 `app/sysbuild/mcuboot.overlay` 删掉。GP8/GP9 那路 UART 在恢复模式下**不参与**。

进恢复有四条入口：应用侧魔数 / `zlrs recovery`（置 boot mode 重启）、复位后 300 ms 窗口收到合法 mcumgr 帧、没有可启动镜像、以及**不用**的按键与 PIN_RESET。换槽用 `swap using offset`，应用把初始化跑完才 `boot_write_img_confirmed()`，没确认的镜像下次复位回滚。机制与踩过的坑见 [01-system.md](01-system.md) 2.4，当前状态见 [08-roadmap.md](08-roadmap.md) M1。

### 3.4 ROM BOOTSEL

芯片级，不跑我们的固件：按住 BOOTSEL 插 USB 出 `RPI-RP2` 盘。不带 bootloader 的变体拖一次 `zephyr.uf2`；带 MCUboot 的变体第一次要两轮（先 bootloader，再 `zephyr.signed.uf2`），之后 ROM 的 U 盘模式不再回来。

### 3.5 bring-up 档（现在的状态）

`CONFIG_ZLRS_APP_RADIO=n` 是构建档位不是运行模式：不初始化 USP/RAC、不提交任何事务，每次事务当场按"发出去、没收到回音"收尾；端口、配置模式、按键、状态灯、NVS 照常。上板自检 `scripts/zlrs-bringup.py` 跑的就是这一档。分层开关与推荐顺序见 [01-system.md](01-system.md) 2.3。

## 4. 烧写路径

| 变体 | 第一次 | 之后 |
| --- | --- | --- |
| 不带 bootloader | 按住 BOOTSEL 拖 `zephyr.uf2`，或 SWD | 同上（没有回滚） |
| 带 MCUboot | 两轮 BOOTSEL：先 bootloader，再 `zephyr.signed.uf2` | 应用侧触发进恢复 + `smpmgr` 上传签名镜像，不用按键 |

一条命令升级：`python3 scripts/zlrs-update.py --port /dev/ttyACM1`。分区数字见 [板级 README](../boards/zlrs/rp2350_zero_core2021/README.md#4-分区与启动)。

## 5. 接线与摆放注意

- 3V3 由 Zero 供 Core2021，两块模块共地。
- SPI 跳线尽量短；GP16 被板载 WS2812B 占着，所以射频 SPI 用 GP4–GP7 而不是 Pico 默认的 GP16–19。
- 天线座那一侧留 15–20 mm 净空（不贴金属、不过线）。
- 按键要自己接：GP12 对地、内部上拉（DT 里配好了），不接就只有魔数 / shell 两个配置入口。
- hf / lf 两套标定表按 SKU 分家，功率表与标定频点不能共用；上板后要按 Core2021 的 PA 网络重标，验收标准见 [08-roadmap.md](08-roadmap.md) 的 M0。

## 6. 这块原型上的限制

| 限制 | 影响 |
| --- | --- |
| 对外只有 USB，没有接插件 | CRSF 与数据口只能用模块焊盘引线；控制台与升级都走 USB |
| 没有用户按键 | 配置模式只能靠数据口魔数或控制台 shell 进入 |
| Core2021 还没接上 | 只能跑 `CONFIG_ZLRS_APP_RADIO=n` 档，射频未验证 |

软件侧的待修/待查项（RP2350 TRNG 会让 USB 死掉、MCUboot 换槽未走通、`--confirm` 的"标记永久"被 bootloader 拒绝）统一记在 [08-roadmap.md](08-roadmap.md) M1。

## 7. 相关文档

| 内容 | 文档 |
| --- | --- |
| 引脚、别名、SKU 标定、分区表的权威定义 | [板级 README](../boards/zlrs/rp2350_zero_core2021/README.md) |
| 模式设计、端点二选一、启动链与升级 | [01-system.md](01-system.md) 2.1 / 2.4 |
| 进度与待办 | [08-roadmap.md](08-roadmap.md) |
| 按键与状态灯语义 | [07-ui.md](07-ui.md) |
