# 原型：RP2350-Zero + Core2021

状态：**桌上原型**——主控与射频两块模块用线连起来，还没有载板。载板 v1
（J1/J2 接插件、机械尺寸）是 [07-carrier.md](07-carrier.md)，仍是草稿。

本文是**现在手里这套硬件**的总览：有哪些物理接口、软件在各种模式下怎么用它们、
烧写与分区怎么走。引脚、SKU 标定与分区表的权威定义在
[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md)，软件模式的设计在
[01-system.md](01-system.md) 2.1/2.4。

## 1. 实物构成

| 部件 | 规格 | 备注 |
| --- | --- | --- |
| 主控 | 微雪 RP2350-Zero：RP2350A（Cortex-M33）、4 MB QSPI flash、原生 USB-C、板载 WS2812B | BOOTSEL 与 RESET 在模块上；不引 SWD |
| 射频 | 微雪 Core2021（Semtech LR2021） | 单颗分时双频：sub-G + 2.4G |
| SKU | **LF**（410–510 MHz） | 地区 CN470(2) / CN433(3)；与 SKU 不匹配时红灯告警且不发射频 |
| 天线 | 433 MHz，接模块天线座 | 2.4G 那一路的实走线按模块图纸核对 |
| 供电 | USB-C 或 5V 输入脚 | Core2021 的 3V3 由 Zero 供 |
| 模块时钟 | 无源晶振，不是 TCXO | 所以 DT 里 `tcxo-*` 保持关断，`reg-mode` 用 DC/DC |
| 现状 | Core2021 还没接上 | 跑 `CONFIG_ZLRS_APP_RADIO=n` 的 bring-up 档：端口、配置模式、升级链已验，射频未验 |

## 2. 物理接口

### 2.1 对外的口

| 接口 | 位置 | 电气 | 软件里的名字 |
| --- | --- | --- | --- |
| USB-C | RP2350-Zero 模块 | 原生 USB 全速，兼供电 | `usbd` + 3 块 CDC；ROM 的 UF2 口 |
| J1（CRSF） | 载板草稿：JST-GH 1.25 6P | DS-009 Telemetry 线序，pin2/3 = RX/TX；pin1 经二极管取 5V；pin4/5 悬空 | `uart0` |
| J2（数据） | 载板草稿：JST-GH 1.25 6P | 同一套线序，pin2/3 = GP9/GP8 | `uart1` |
| 天线座 | Core2021 模块 | sub-G 射频收发 | `lora_core2021` |
| BOOTSEL / RESET | RP2350-Zero 模块 | 芯片级按钮，不是 GPIO | bootrom / 复位 |
| 按键 SW1 | 载板草稿：GP12 ↔ GND | 对地、内部上拉；**本板没接** | `zlrs-button` |
| WS2812B | GP16（板上自带） | 单像素 RGB，PIO0 驱动 | `zlrs-led` |

### 2.2 板内连线（Zero ↔ Core2021）

只在 `CONFIG_ZLRS_APP_RADIO=y` 时使用；`=n` 时整块射频不碰（含 SPI 与三根
控制线），见 `app/src/radio_null.c`。

| 信号 | RP2350-Zero | Core2021 | 说明 |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP6 / GP7 / GP4 | 对应脚 | 总线 16 MHz，`spi-max-frequency` 16 MHz |
| SPI0 CS | GP5 | 对应脚 | 普通 GPIO 片选（`cs-gpios`），不用硬件 CSN |
| BUSY | GP20 | BUSY | 高有效，内部上拉 |
| NRESET | GP21 | NRESET | 低有效 |
| DIO8（IRQ） | GP22 | DIO8 | 高有效，内部下拉 |
| 3V3 / GND | 3V3、GND | 3V3、GND | 由 Zero 供 |

### 2.3 GPIO 占用总表

| GPIO | 用途 | 引出到 |
| --- | --- | --- |
| GP0 / GP1 | UART0 TX / RX，CRSF，420000 | J1 pin3 / pin2 |
| GP4 / GP5 / GP6 / GP7 | SPI0 MISO / CS / SCK / MOSI | Core2021 |
| GP8 / GP9 | UART1 TX / RX，数据口，默认 115200（NVS 可改） | J2 pin3 / pin2 |
| GP12 | 按键（预留，本板没接） | 载板 SW1 |
| GP16 | WS2812B 状态灯 | 板上 |
| GP20 / GP21 / GP22 | BUSY / NRESET / DIO8 | Core2021 |

其余 GPIO 未用。USB 不占 GPIO（原生 USB 控制器），但它是三条链路的第二组端点与
唯一控制台，见下节。

## 3. 软件模式 × 接口用法

| 接口 | 应用模式 | 配置模式 | MCUboot 恢复 | ROM BOOTSEL |
| --- | --- | --- | --- | --- |
| USB-C | 3 块 CDC：CRSF / 数据 / 控制台 | 同左；控制台可敲 shell | 2 块 CDC：恢复 + 控制台，CRSF 那块删掉 | `RPI-RP2` 大容量盘，无 CDC |
| UART0 GP0/1 | CRSF 收发 420000 | 端口映射不变，但超帧停，不转发 | 不初始化 | 不跑固件 |
| UART1 GP8/9 | 字节透传 | 改跑配置协议 | **不用**（恢复走 USB） | 不跑固件 |
| 射频 SPI / BUSY / DIO8 | 每帧四个时隙收发 | 不发事务 | 不碰 | 不跑固件 |
| 按键 GP12 | 运行中长按 ≥1 s 配对 | 会话里不看按键 | 刻意不用（`BOOT_SERIAL_ENTRANCE_GPIO=n`） | BOOTSEL 是另一颗芯片级按钮 |
| 状态灯 | 颜色 + 节奏见 [06-ui.md](06-ui.md) | 白灯 0.5 Hz | MCUboot 不驱动，保持复位前最后一帧颜色 | 不驱动 |
| 看门狗 wdt0 | 默认关；开了主循环喂 | 会话期间 `disarm`，退出再 `arm` | `lib/bootguard` 先清硬件 `ENABLE` 位 | — |

### 3.1 应用模式（正常收发）

两条逻辑链路（CRSF、数据），每条都有两块端点二选一：主机的 CDC 被打开（DTR 有效）
**或**该端点收到过主机字节，就走 USB；否则走 UART。第二条判据是给 usbipd 与虚拟化
补的——`SET_CONTROL_LINE_STATE` 不一定送得到设备，只认 DTR 的话 CDC 永远选不中。
选择在每次读之前重算，ISR 先记"收到过字节"再判端点，所以第一帧不会被滞后吃掉；
非当前端点的进账直接丢。实现在 `app/src/ports.c` 的 `port_active()`。

控制台固定钉在 USB 第三块 CDC（`zephyr,console` = `cdc_acm_uart2`），不参与二选一；
数据口上的控制魔数是 `zlrs!configmode!`（进配置）与 `zlrs!bootloader!`（进恢复），
只有匹配到前缀的字节被扣住，正常数据不丢不重（`app/src/main.c`）。

### 3.2 配置模式

数据口切换成配置协议（读状态、触发配对、写根密钥、写失控值、改数据口波特率），
`EXIT` 或空闲超时（默认 300 s）退出；写进去的密钥与失控值当场生效。三个入口、
各自一个 Kconfig：

| 入口 | 开关 | 默认 | 本板能不能用 |
| --- | --- | --- | --- |
| 开机按住按键 | `CONFIG_ZLRS_APP_CONFIG_MODE` | y | 不能：GP12 没接按键 |
| 数据口魔数 | `CONFIG_ZLRS_APP_CONFIG_MAGIC` | n | 能，上板验证走的就是这条 |
| 控制台 shell `zlrs config` | `CONFIG_ZLRS_APP_SHELL` | n | 能（`zlrs status` / `zlrs ports` / `zlrs recovery` 同在此开关下） |

三个入口进的是同一个会话，而且都在主循环里跑（shell 只置一个请求）：会话期间超帧
暂停、数据口不会被透传抢字节；会话开始先 `zlrs_watchdog_disarm()`（写短语要几十秒
PBKDF2，会被 8 s 看门狗复位），退出再武装。

### 3.3 MCUboot 串口恢复

只走 USB。bootloader 模式下 USB 只剩两块 CDC：第 1 块（= 应用的数据口那块
`cdc_acm_uart1`，`zephyr,uart-mcumgr`）收 mcumgr 帧，第 2 块是内核日志；CRSF 那块
由 `app/sysbuild/mcuboot.overlay` 删掉。GP8/GP9 那路 UART 在恢复模式下**不参与**。

进恢复有四条入口：应用侧魔数 / `zlrs recovery`（置 boot mode 重启）、复位后 300 ms
窗口收到合法 mcumgr 帧、没有可启动镜像、以及**不用**的按键与 PIN_RESET。换槽用
`swap using offset`，应用把初始化跑完才 `boot_write_img_confirmed()`，没确认的镜像
下次复位回滚。细节与已知问题见 [01-system.md](01-system.md) 2.4。

### 3.4 ROM BOOTSEL

芯片级，不跑我们的固件：按住 BOOTSEL 插 USB 出 `RPI-RP2` 盘。不带 bootloader 的
变体拖一次 `zephyr.uf2`；带 MCUboot 的变体第一次要两轮（先 bootloader，再
`zephyr.signed.uf2`），之后 ROM 的 U 盘模式不再回来。

### 3.5 bring-up 档（现在的状态）

`CONFIG_ZLRS_APP_RADIO=n` 是构建档位不是运行模式：不初始化 USP/RAC、不提交任何事务，
每次事务当场按"发出去、没收到回音"收尾；端口、配置模式、按键、状态灯、NVS 照常。
上板自检 `scripts/zlrs-bringup.py` 跑的就是这一档。分层开关与推荐顺序见
[01-system.md](01-system.md) 2.3。

## 4. 分区与烧写

| | 不带 bootloader | 带 MCUboot |
| --- | --- | --- |
| flash 布局 | code 1 MB @ 0x0 + storage 3 MB @ 0x100000 | mcuboot 128 KB @ 0x0 + slot0 832 KB @ 0x20000 + slot1 832 KB @ 0xF0000 + storage 2.25 MB @ 0x1C0000 |
| 启动链 | RP2350 bootrom → 应用 | RP2350 bootrom → MCUboot（验签）→ slot0 |
| 升级 | BOOTSEL 拖 `zephyr.uf2`，或 SWD | 第一次两轮 BOOTSEL；之后串口恢复，不用按键 |
| 回滚 | 无 | 没确认的镜像下次复位换回去；没有可启动镜像就停在恢复模式 |

USB 上两种模式自报的名字不同：应用是 `ZLRS`、bootloader 是 `ZLRS MCUBOOT`，脚本按
`/dev/serial/by-id/...` 找口，不写死 `ttyACM*` 编号。一条命令升级：
`python3 scripts/zlrs-update.py --port /dev/ttyACM1`。

## 5. 电气与布局约束（原型阶段）

- Core2021 的 3V3 由 Zero 供；两块模块共地。
- SPI 走线尽量短（载板目标 < 20 mm）；GP16 被板载 WS2812B 占着，所以射频 SPI 用
  GP4–GP7 而不是 Pico 默认的 GP16–19。
- 天线净空：载板草稿要求在模块天线座那一侧留 15–20 mm，不铺铜、不走线、不放大件。
- J1/J2 的 5V 各自经肖特基二极管或在一起，两个口同时接电源不反灌（载板草稿）。
- hf / lf 两套标定表按 SKU 分家，功率表与标定频点不能共用；上板后要按 Core2021 的
  PA 网络重标，验收标准见 [05-roadmap.md](05-roadmap.md) 的 M0。

## 6. 已知问题（硬件相关）

| 问题 | 现状 | 影响 |
| --- | --- | --- |
| RP2350 TRNG 会让 USB 死掉 | 默认关（`CONFIG_ENTROPY_RPI_PICO_RNG=n`，`ZLRS_APP_ENTROPY_TRNG` 才开） | **量产前必修**；CSPRNG 暂走 Zephyr 默认路径 |
| MCUboot 换槽（`swap using offset`）没走通 | 调试期用 `zlrs-update.py --confirm`（直接写 slot0，无回滚） | 换槽现场要上 SWD 看，见 [05-roadmap.md](05-roadmap.md) M1 |
| 多块 CDC 的接收端装死 | `CONFIG_USBD_CDC_ACM_BUF_POOL=y`（每块 CDC 自己的池） | 不加就是"只能发不能收"且无提示 |
| 本板没有用户按键 | 配置只能走数据口魔数或控制台 shell | 配好 `CONFIG_ZLRS_APP_CONFIG_MAGIC` / `..._SHELL` 才有无按键入口 |

## 7. 相关文档

| 内容 | 文档 |
| --- | --- |
| 引脚、SKU 标定、分区表的权威定义 | [板级 README](../boards/zlrs/rp2350_zero_core2021/README.md) |
| 模式设计、端点二选一、启动链与升级 | [01-system.md](01-system.md) 2.1 / 2.4 |
| 按键与状态灯语义 | [06-ui.md](06-ui.md) |
| 载板 v1 的接插件与机械规格（草稿） | [07-carrier.md](07-carrier.md) |
| LR2021 的速率、时序与 SPI/IRQ 摘要 | [04-lr2021.md](04-lr2021.md) |
