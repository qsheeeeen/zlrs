# 平台、端口与固件

本文写"这份固件跑在什么上、对外怎么用、怎么裁、怎么升级"，以及跨层的工程约定。系统怎么工作见 [03-architecture](03-architecture.md)，空中协议见 [04-protocol](04-protocol.md)，安全见 [05-security](05-security.md)。

## 1. 硬件与软件基线

| 项目 | 选择 |
| --- | --- |
| 射频 | Semtech LR2021（QFN32），SPI + BUSY + DIO；能力摘要见 [06-radio](06-radio.md) |
| 参考硬件 | 微雪 RP2350-Zero（RP2350A，4 MB flash）+ 微雪 Core2021（LR2021）。sub-G 分 HF（850–930 MHz）与 LF（410–510 MHz）两个 SKU |
| 板级定义 | `boards/zlrs/rp2350_zero_core2021/`：引脚、别名、两个 SKU 的标定表、flash 分区与构建目标，见[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md) |
| 射频栈 | `usp_zephyr` + `usp` v1.1.2-feature-202604，使用 RAC/RALF 层 |
| Zephyr | v4.4.0（RP2350 的 TRNG 驱动从这一版起在树内） |
| 仓库拓扑 | T2：zlrs 自己当 manifest 仓库并存放应用 |

不用 LBM（LoRa Basics Modem）——那是 LoRaWAN 那套；我们要的是裸包收发，即 RAC（Radio Access）层。相关能力在 `smtc_rac_flrc.c` / `smtc_rac_fsk.c` / `smtc_rac_lora.c`。

## 2. 端口与工作模式

**TX 与 RX 是同一套硬件、同一份固件**，差别只在角色配置。这是全项目最重要的一条简化：用户只记一套操作，文档、脚本与主机侧用例也各只有一份。

端口设计有两条：

1. **没有专门的控制口。** 配置和透传复用同一条数据通路，靠工作模式区分：正常模式数据口是透传字节流，配置模式改跑配置协议。CRSF 端口在两种模式下都不变。
2. **每条链路两块端点，二选一。** CRSF 与数据各有一路 UART 加一块 USB CDC。主机把这链路的 CDC 打开（DTR 有效）就走 USB，否则走 UART；控制台固定在 USB 第三块 CDC（`zephyr,console`）上，不参与二选一。

"主机在用这块端点"有两条证据：CDC 的 **DTR**（标准做法），以及**这块端点收到过主机字节**。第二条是上板补的：`SET_CONTROL_LINE_STATE` 在 USB/IP 转发（usbipd）与部分虚拟化场景里不一定送得到设备，只看 DTR 的话 CDC 永远选不中，主机发来的字节会被当"非当前端点"静静丢掉——配置协议、透传、CRSF 输入全都收不到，而且没有任何报错。ISR 在判断当前端点**之前**先记下"收到过字节"，所以第一帧不会被端点选择的滞后吃掉（`app/src/ports.c` 的 `port_active()`）。

物理接口清单、板内连线与各模式下的完整用法见 [07-prototype](07-prototype.md) 第 2、3 节；引脚与别名的权威定义见[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md)。

### 按键的时机决定语义

| 时机 | 行为 |
| --- | --- |
| **开机时按住按键** | 进入配置模式 |
| **开机后长按按键** | 配对 |

按键语义的细节（没有短按、换绑、边界行为）见 [09-ux](09-ux.md)。配置模式在超时（默认 300 s）或收到明确的退出指令后结束。

### 没有按键时的两条入口

板上没有用户按键时（例如现在的原型：只有 BOOTSEL 与 RESET），配置模式还有两条入口，各自一个开关、默认关：

| 入口 | 开关 | 用法 |
| --- | --- | --- |
| 数据口魔数 | `CONFIG_ZLRS_APP_CONFIG_MAGIC` | 数据口收到 16 字节 `zlrs!configmode!` 就进配置模式，`EXIT`/超时回正常模式（与 `zlrs!bootloader!` 共用一套前缀匹配，正常字节不丢） |
| 控制台 shell | `CONFIG_ZLRS_APP_SHELL` | 控制台 CDC（USB 第三块）上敲 `zlrs config`；另有 `zlrs status` / `ports` / `recovery` |

三个入口进的是同一个会话，而且都在主循环里跑（shell 只置一个请求，主循环看到才开会话）：会话期间超帧暂停，数据口不会被透传抢字节，也不会出现"边透传边配置"。

配对同理：没有按键时用配置工具的 `pair` 命令触发（`scripts/zlrs-config.py`）。

代价是 TX 也不能在运行时边透传边配置——进配置会话就暂停收发，`EXIT` 或超时后继续。换来的是 TX/RX 完全相同的流程，以及配置模式**不可能被远程触发**（对安全有实际价值，见 [05-security](05-security.md) 第 10 节）。

## 3. 特性可裁剪

一份固件要能适配多种硬件配置，这是硬要求而不是加分项：

- **USB 必须可裁掉**——RX 轻量版没有原生 USB
- 数据 UART 也应是可选的
- 用 Kconfig 与 devicetree 表达能力，应用层靠 DT 探测，**不要到处 `#ifdef`**

本板每条链路都是 UART + CDC 两块端点，控制台在 USB 第三个 CDC。轻量板把 `zlrs-data-cdc`（或整个 `&usbd`）删掉即可；别名一个都不挂就是"这条链路的端口不存在"，一样编得过。

**USB 设备栈由镜像自己建**（`lib/usb/usb_dev.c`，开关 `CONFIG_ZLRS_USB`，应用侧是 `CONFIG_ZLRS_APP_USB`）。Zephyr 的 device_next 栈里 devicetree 的 `zephyr,cdc-acm-uart` 节点只表达"要几块 CDC"，设备不会自己上线：镜像得建 usbd context、把类注册进配置、再 `usbd_enable()`。Zephyr 自带的 `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` 只注册**第一块** CDC，本板应用三块（bootloader 两块），没注册的那块没有 context，第一次往它写日志就是 NULL 解引用、整机停住——所以那个开关保持关着。应用与 bootloader 共用这一份代码，差别只在字符串与 VID/PID（`zlrs` / `ZLRS` 与 `zlrs` / `ZLRS MCUBOOT`）。

**CDC 的接收端要用自己的缓冲池**（`CONFIG_USBD_CDC_ACM_BUF_POOL=y`）。全速设备上这个开关默认是关的，多块 CDC 会去抢 UDC 的公共池；RX 工作项一旦分配失败就再也不重试（`CDC_ACM_RX_FIFO_BUSY` 位不清），表现是"串口只能发不能收"，而 CDC 驱动的日志又因为控制台挂在 CDC 上被强制关掉、一点提示都没有——上板第一次就是这么被坑的，见 [10-roadmap](10-roadmap.md) 的 M1。

库层的开关（`lib/*/Kconfig`）由应用的功能开关反选：`ZLRS_APP_CONFIG_MODE` / `ZLRS_APP_STATUS_LED` / `ZLRS_APP_CRSF_PORT` 一关，对应的 `ZLRS_CONFIG` / `ZLRS_LED` / `ZLRS_CRSF` 就可以一起裁掉（应用里留了退化实现，编得过）。协议核心、链路层、地区配置层与 CRSF 编解码是应用永远需要的，由 `ZLRS_APP_CORE` 固定选中。

## 4. 上板调试的分层开关

硬件第一次上电时不要一次把所有东西都打开。每一项复杂功能都有自己的 Kconfig 开关，从最简单的形态往上逐层验证：每关掉一层，剩下的还能跑通就说明问题不在被关掉的那一层。关掉的项在开机时会打一条 `bring-up:` 警告，日志里能看出这台设备不是生产形态。

| 开关 | 关掉之后 | 主机侧用例 |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_SECURITY_PROFILE_P0=y` | 帧不带 tag，先把收发时序与帧格式跑通 | `link: bring-up without tag (P0)` |
| `CONFIG_ZLRS_APP_FHSS=n` | 固定发在同步信道，不跳频 | `link: bring-up without hopping` |
| `CONFIG_ZLRS_APP_SESSION_HANDSHAKE=n` | 配对级钥匙直接当会话钥匙：BEACON 之后只回一条 `SESS_RESP` | `link: bring-up without session handshake` |
| `CONFIG_ZLRS_APP_TELEMETRY=n` | 控制链路只剩下行 RC（TX 的失联判定一并关掉，否则它会一直重建会话） | `link: bring-up without telemetry` |
| `CONFIG_ZLRS_APP_DATA_ARQ=n` | 透传回到 best-effort（默认就是关） | `link: arq *`、`link: backpressure` |
| `CONFIG_ZLRS_APP_RATE_ADAPT=n` | 固定速率档（默认就是关） | `link: rate adapt` |
| `CONFIG_ZLRS_APP_DYNAMIC_POWER=n` | 固定发射功率 | `power: *` |
| `CONFIG_ZLRS_APP_RESYNC_PERIOD_MS=0` | 不做连接态周期 `BEACON`：跳频索引只在建链时对齐，对端重启也不会自动重连 | `link: hop resync`、`link: peer restart reconnect` |
| `CONFIG_ZLRS_APP_CONTROL_MODULATION_FLRC=y`（默认） | 控制链路跑 FLRC；换成 `..._LORA=y` 则进 LoRa 远距档（超帧周期跟着变长） | `link: lora control modulation`、`region: lora control plan` |
| `CONFIG_ZLRS_APP_LINK_CONTROL=n` / `ZLRS_APP_LINK_DATA=n` | 只跑一条链路：先把单条链路（含射频时序）跑通，再合起来看分时 | 编译验证（两个 SKU 都在 CI 里） |
| `CONFIG_ZLRS_APP_RADIO=n` | 射频整块不参与：不初始化 USP/RAC、不提交任何事务，每次事务当场按"发出去、没收到回音"收尾（`app/src/radio_null.c`）。端口、配置模式、按键、状态灯、NVS 照常跑，适合 Core2021 还没接上时先验证射频以外的部分 | 编译验证 + 上板（USB/端口/配置模式） |
| `CONFIG_ZLRS_APP_CRSF_PORT=n` | 不初始化 CRSF 端口：TX 只发配置好的失控值 | 编译验证 |
| `CONFIG_ZLRS_APP_CONFIG_MODE=n` / `ZLRS_APP_PAIRING_BUTTON=n` / `ZLRS_APP_STATUS_LED=n` | 关掉配置模式 / 按键配对 / 状态灯 | 编译验证 |

推荐的顺序：`P0` + 关跳频 + 关握手 + 关遥测 → 先确认"自己发的帧自己能收到"；然后打开 tag（`P1`）；再打开跳频；再打开遥测与失联判定；最后才是速率自适应与动态功率——这两项的阈值本来就要等实测曲线（见 [10-roadmap](10-roadmap.md) 的 M4），上板第一步不该被它们干扰。

每个开关都对应一条主机侧用例，断言"关掉之后链路照常工作"：这套用例跑在 `tests/host`，不需要硬件。

## 5. 启动链与固件升级

板级有两个目标家族：`.../m33/hf`、`.../m33/lf`（应用直接放在 flash 里）与 `.../m33/hf/mcuboot`、`.../m33/lf/mcuboot`（多一个 bootloader 与两槽固件分区）。两者共用同一份应用代码，差别只在分区表与是否带 bootloader；分区数字见[板级 README](../boards/zlrs/rp2350_zero_core2021/README.md#4-分区与启动)。

| | 不带 bootloader | 带 MCUboot |
| --- | --- | --- |
| 启动链 | RP2350 bootrom → 应用 | RP2350 bootrom → MCUboot（校验签名）→ slot0 里的应用 |
| 升级 | 按住 BOOTSEL 拖 `zephyr.uf2`（或 SWD） | 第一次两轮 BOOTSEL（先 bootloader、再 `zephyr.signed.uf2`）；之后走串口恢复，不用按键 |
| 回滚 | 无 | `swap using offset`：升级失败可以退回上一版 |

**为什么上 MCUboot。** 三件事：① 现场升级不需要拆机、不需要调试器；② 镜像签名（ECDSA-P256）+ 启动校验，坏了能回滚；③ 砖机也能救——bootloader 起来后没有可启动的镜像就停在串口恢复里。

### 恢复通道只走 USB

bootloader 自己收 mcumgr 帧，写进 slot 后由 MCUboot 换槽。USB 上只留两块 CDC（应用是三块）：

| bootloader 模式 | 对应应用的哪一块 | 用途 |
| --- | --- | --- |
| 第 1 块 CDC | 数据口（`cdc_acm_uart1`） | 串口恢复：mcumgr 帧走这里（chosen `zephyr,uart-mcumgr`） |
| 第 2 块 CDC | 控制台（`cdc_acm_uart2`） | 内核日志（chosen `zephyr,console`） |

CRSF 那块 CDC 在 bootloader 里没有用处，直接从 DT 里删掉（`app/sysbuild/mcuboot.overlay`）。好处是升级只需要插板载 USB-C，不用 UART 转接；代价是**端口号会变**：应用模式是三块 CDC（CRSF/数据/控制台），bootloader 模式只有两块。两种模式在 USB 上自报的名字不同——应用是 `ZLRS`、bootloader 是 `ZLRS MCUBOOT`（`CONFIG_ZLRS_USB_PRODUCT`）——所以脚本按 `/dev/serial/by-id/...` 里的名字找端口，不写死 `ttyACM*` 编号。

### 进恢复的四条入口

都不碰 BOOTSEL：

| 入口 | 什么时候生效 | 说明 |
| --- | --- | --- |
| 应用侧触发 | 设备正常运行中 | 数据口收到 16 字节魔数 `zlrs!bootloader!`（`scripts/zlrs-recovery.py`）或 shell 的 `zlrs recovery` → 应用置 boot mode 并重启 → bootloader 读到标志就停在恢复模式。**升级一台健康设备不用碰设备本身** |
| 启动窗口 | 每次复位后 300 ms | 窗口里收到**合法的 mcumgr 帧**就停下来升级——主机脚本先开始发 `image list`，再让设备复位（拔插 USB 或按复位键） |
| 没有可启动镜像 | 砖机 | 校验失败/镜像为空时停在恢复模式，永远不会"转圈" |
| 按键 | 不用 | bootloader 不占板上按键：它在应用里有"开机按住进配置"的语义，抢占会让两边都变歧义 |

boot mode 那块标志放在 SRAM 起始处的 4 字节里（Zephyr 树里 RP2350 专用的 `rp2350-boot-mode-retention.dtsi`，`sram0` 相应让开 4 字节），由 `RETENTION_BOOT_MODE` 读写，应用与 bootloader 看到的是同一份 DT。魔数匹配是逐字节的：只有当前缀的那几个字节会被扣住，一旦不匹配就原样交回透传队列，所以正常数据既不丢也不重复。这个入口在 Kconfig 里是 `CONFIG_ZLRS_APP_UPDATE_TRIGGER`，量产/竞速构建可以关掉。

`CONFIG_BOOT_SERIAL_PIN_RESET`（"复位由引脚引起就进恢复"）**故意没开**：RP2350 的 hwinfo 把 POWMAN 的 `HAD_RUN` 位映射成 `RESET_PIN`，这一位在"芯片跑过"这件事上的语义比"按了复位键"宽，没上板确认之前开它就有"每次上电都停在恢复模式"的风险。

主机侧一条命令就够（完整用法见 [README](../README.md)）：

```shell
python3 scripts/zlrs-update.py --port /dev/ttyACM1
```

它按顺序做四件事：① 跑 `scripts/zlrs-recovery.py` 触发；② 等 bootloader 的恢复口（按 USB 名字找，`--recovery-port` 可指定）；③ 用 `smpmgr upgrade` 上传 `zephyr.signed.bin`、标记下次启动并重启（`--client none` 只打印命令）；④ 等应用的 `ZLRS` 端口回来报成功。

**密钥。** 开发期用 MCUboot 自带的测试密钥（`bootloader/mcuboot/root-ec-p256.pem`），bootloader 校验与应用签名用的是同一把——由 sysbuild 统一设置，两边不会不一致。量产前生成自己的密钥（`imgtool genkey -t ecdsa-p256`），私钥不进仓库，并且**要重新烧一次 bootloader**：公钥在 bootloader 里。

### 镜像什么时候算"好"：确认位 + 复位

回滚的判据只有一位——镜像有没有被确认：

| 环节 | 谁做 | 配置 |
| --- | --- | --- |
| 打标志 | 应用把该初始化的都初始化完（端口、NVS、链路、USB 都起来了）就调 `boot_write_img_confirmed()` | `CONFIG_ZLRS_APP_MCUBOOT_CONFIRM`（默认开，`..._CONFIRM_MS` 可再要一段观察窗口） |
| 回滚 | 升级用 test 语义（`smpmgr upgrade` 默认）：换进来的镜像没确认就崩，**下一次复位**时 MCUboot 回滚到上一版；没有上一版就停在串口恢复（`BOOT_SERIAL_NO_APPLICATION`） | bootloader 侧，不用配 |
| 让复位发生 | 应用崩了是 CPU 停住（这块板上 Zephyr 没有 `RESET_ON_FATAL_ERROR`），所以要么手动复位/断电，要么打开看门狗让它自己复位 | `CONFIG_ZLRS_APP_WATCHDOG`（默认**关**，调试期建议开；`..._TIMEOUT_MS` 默认 8 s） |

看门狗和回滚是两件事：看门狗只负责"把卡死变成复位"，判据始终是那个确认位。打开时它在 `PRE_KERNEL_1` 就武装（紧跟 wdt 驱动），所以连**驱动初始化阶段**的挂死也能被复位；喂狗只在主循环与配置模式的轮询循环里做，主循环卡住同样算死。上板调试开着的实际好处：刷坏的镜像自己复位 → MCUboot 回滚 → 停在串口恢复 → 直接再从 USB 上传，全程不用按 BOOTSEL。

Bootloader 侧有个对应的护栏（`lib/bootguard`，`CONFIG_ZLRS_BOOTGUARD`，bootloader 镜像默认开）：MCUboot 起来第一件事就是把看门狗**关掉**。原因是 RP2350 的看门狗在一次看门狗复位之后 `ENABLE` 位通常还是 1（它会按周期一直复位，直到软件清掉），那样 MCUboot 刚跑换槽/回滚就被 8 秒后的下一次复位打断——回滚永远做不完，现象是设备在"USB 描述符请求失败 / 掉线"之间反复。护栏先把这一环掐断，进应用后再由应用自己决定要不要重新武装。

**看门狗与串口恢复。** 两处必须一起做，缺一个都会踩坑（上板各踩过一次）：

1. **应用侧**：`zlrs recovery` / 数据口魔数触发时，重启前先 `zlrs_watchdog_disarm()`（`app/src/watchdog.c`）。这时驱动状态里看门狗是 enabled，`wdt_disable()` 真的有效；不关的话它会跟着重启跑进 bootloader。
2. **bootloader 侧**：护栏**不能**用 `wdt_disable()`——驱动的实现是 `if (data->enabled == false) return -EFAULT;`（复位后驱动状态是全新一局，硬件却还开着），所以 `lib/bootguard` 直接清 `watchdog_hw->ctrl` 的 `ENABLE` 位。

漏掉任一侧的现象都一样：MCUboot 进了串口恢复、**几秒后被看门狗复位打断**（上板实测只有 ~1.5 s 窗口），主机这边就是"恢复口闪现一下、镜像永远传不完"。

## 6. 错误处理约定

上板调试最贵的教训是"**错误被吞掉**"：设备看起来在跑，实际某个端口/写入早就没工作，日志里一点痕迹都没有（CDC 抢占缓冲池让 RX 静默失败、`nvs_write()` 返回值判错、看门狗把恢复打断，都属于这一类）。约定如下：

| 层 | 能拿到什么 | 该做什么 |
| --- | --- | --- |
| `lib/`（纯 C，主机可测） | 只有输入输出，没有设备与日志 | 返回错误码/布尔值，**不打印**；密码学一律 fail closed（失败输出全零，认证自然不过） |
| `app/src/`（Zephyr 胶水） | 设备、日志、策略 | 每个能判的返回值都要判，然后**按后果选策略**：退化（端口不可用 → 走另一块端点）、降级（NVS 不可用 → 不发射频）、还是致命（PSA 初始化失败 → `fault_loop`） |
| `lib/usb`、`lib/bootguard` 等镜像级胶水 | 失败影响整机可用性 | 同样判，但**默认不致命**：USB 是可裁的（第 3 节），起不来就报警继续跑，别让只能跑串口的板子陪葬 |

几条具体规则：

- **返回值判到语义那一步**，不是判个"非零"。例：`nvs_write()` 成功返回写入字节数、内容一致返回 0、出错返回负 errno——按 `!= 0` 判错会把每次成功都当成失败；只判 `< 0` 又会把"长度对不上"当成成功。现在的写法是 `rc == 0 || rc == len` 才算成功。
- **忽略返回值要写清理由**，通常只有两类：best-effort（透传/CRSF 发送，丢了有计数：`serial_drop`、`air_drop`、`crsf_drop`）与清理路径（`psa_destroy_key`、`wdt_disable`）。
- **API 返回 void 时，得有别的机制暴露失败**：`uart_irq_rx_enable()` 判不了，就要靠"回调是否装上"（`uart_irq_callback_user_data_set()` 返回 int，必须判）+ 上层超时/计数。
- **每帧都在跑的路径不许刷日志**：失败只报一次（`led_port` 的灯带刷新），或者只更新计数器由状态命令/周期日志呈现。
