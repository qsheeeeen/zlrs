# 固件行为

本文写固件对外表现出的行为：USB 与端口、工作模式、运行期配置、特性裁剪、启动与升级、错误处理约定。架构见 [07-architecture](07-architecture.md)，协议见 [09-protocol](09-protocol.md)，安全见 [10-security](10-security.md)，调试相关见 [12-debug](12-debug.md)。

## 1. USB 与端口

固件默认输出三块 CDC：

| CDC | 用途 | 对应 UART 端点 |
| --- | --- | --- |
| CRSF | CRSF 遥控链路 | UART0（GP0/GP1，420000） |
| 数据 | 数传透传 | UART1（GP8/GP9，默认 115200） |
| **配置** | 配置协议：进配置模式、读写参数 | — |

调试构建再多一块 **console**（内核日志 + shell，见 [12-debug](12-debug.md)）。

两种模式在 USB 上自报的名字由 `CONFIG_ZLRS_USB_PRODUCT` 决定：应用是 `ZLRS`、bootloader 是 `ZLRS MCUBOOT`，主机脚本按 `/dev/serial/by-id/...` 找口。

**每条链路两块端点，二选一**：主机把这链路的 CDC 打开（DTR 有效）**或**该端点收到过主机字节，就走 USB；否则走 UART。第二条判据是给 USB/IP 转发（usbipd）与虚拟化场景补的——`SET_CONTROL_LINE_STATE` 不一定送得到设备，只认 DTR 的话 CDC 永远选不中，主机发来的字节会被当"非当前端点"静静丢掉，而且没有任何报错。ISR 在判断当前端点**之前**先记下"收到过字节"，所以第一帧不会被端点选择的滞后吃掉（`app/src/ports.c` 的 `port_active()`）。

两条链路的端点二选一互相独立；配置 CDC 不参与透传，只跑配置协议。

## 2. 工作模式

| 模式 | 链路 | 配置 CDC | 说明 |
| --- | --- | --- | --- |
| **应用** | 正常收发 | 空闲 | 常态 |
| **配置** | **射频完全停**（不收不发） | 跑配置协议 | 由配置工具发命令进入，`EXIT` / 超时退出 |
| 串口恢复 | 不跑应用 | — | bootloader 模式，见第 6 节 |

配置模式期间：

- 射频不收也不发，链路状态冻结；
- 数据口收到的字节**直接丢弃并计数**，不做缓存；退出后恢复正常收发；
- 状态灯白灯 0.5 Hz（见 [11-ux](11-ux.md)）；
- 看门狗在会话期间 `disarm`（写短语要几十秒 PBKDF2，会被 8 s 看门狗复位），退出后重新武装。

## 3. 运行期配置

**所有用户可见的设置都在运行期配置，不需要重编固件**：

| 参数 | 说明 |
| --- | --- |
| 根密钥 / 短语 | 短语在设备侧走 PBKDF2 派生根密钥；也可以直接写 64 位 hex 根密钥 |
| 失控值 | 22 字节 CRSF 通道载荷，配对时下发给对端 |
| 数据口波特率 | 下次启动生效 |
| 地区 | 0 = FCC915、1 = EU868、2 = CN470、3 = CN433；写入时校验本机 SKU，不匹配拒绝（见 [09-protocol](09-protocol.md) 5.2）；**下次启动生效** |
| 发射功率上限 | 不超过所选地区的法规上限；**下次启动生效** |
| 配对 | 触发配对（立即）；擦除配对（下次启动生效） |
| 恢复出厂 | 清 NVS（密钥、配对、失控值、波特率、地区、功率）；下次启动生效 |

角色（TX / RX）、频段形态与调制档仍是编译期配置（`CONFIG_ZLRS_APP_ROLE_TX`、`BAND_ROLE_*`、`CONTROL_MODULATION_*`）：它们决定固件跑哪套时序，不属于"用户参数"。

## 4. 特性裁剪

一份固件要能适配多种硬件配置，这是硬要求：

- **USB 必须可裁掉**——轻量版没有原生 USB；
- 数据 UART 也应是可选的；
- 用 Kconfig 与 devicetree 表达能力，应用层靠 DT 探测，不写满 `#ifdef`。

做法：应用只认别名（`zlrs-crsf-uart` / `zlrs-crsf-cdc` / `zlrs-data-uart` / `zlrs-data-cdc` / `zlrs-button` / `zlrs-led`），别名不挂就是"这条链路的端口不存在"，一样编得过。库层开关由应用开关反选：`ZLRS_APP_CONFIG_MODE` / `ZLRS_APP_STATUS_LED` / `ZLRS_APP_CRSF_PORT` 一关，对应的 `ZLRS_CONFIG` / `ZLRS_LED` / `ZLRS_CRSF` 一起裁掉。

**USB 设备栈由镜像自己建**（`lib/usb`，`CONFIG_ZLRS_USB`，应用侧 `CONFIG_ZLRS_APP_USB`）：devicetree 里的 `zephyr,cdc-acm-uart` 节点只表达"要几块 CDC"，设备不会自己上线——镜像要建 context、注册类、再 `usbd_enable()`。Zephyr 自带的 `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` 只注册第一块 CDC，本固件有好几块，所以那个开关保持关着。

**每块 CDC 用自己的缓冲池**（`CONFIG_USBD_CDC_ACM_BUF_POOL=y`）：全速设备上多块 CDC 会抢 UDC 的公共池，RX 工作项分配失败后不会重试，表现是"只能发不能收"且无提示。

## 5. 按键

按键只有一个动作：**长按触发配对**（默认 1 s）。按键是标配，载板上有一颗（SW1）；原型上用杜邦线把 GP12 短接到 GND 模拟。语义细节见 [11-ux](11-ux.md)。

## 6. 启动、恢复与升级

启动链与分区见 [03-platform](03-platform.md) 第 4 节。

进串口恢复有三条入口（都不用按键）：

| 入口 | 什么时候生效 |
| --- | --- |
| 应用侧触发 | 配置协议里的"进恢复"命令（调试构建也可以用 shell 的 `zlrs recovery`）→ 置 boot mode 并重启 |
| 启动窗口 | 每次复位后 300 ms 内收到合法 mcumgr 帧 |
| 没有可启动镜像 | 校验失败 / 镜像为空，停在恢复模式 |

boot mode 标志放在 SRAM 起始处的 4 字节（retained RAM），应用与 bootloader 共用同一份 DT。`CONFIG_BOOT_SERIAL_PIN_RESET` 故意没开：RP2350 把 POWMAN 的 `HAD_RUN` 位映射成 `RESET_PIN`，语义比"按了复位键"宽。

恢复通道**只走 USB**：bootloader 模式下 USB 只有两块 CDC（第 1 块恢复、第 2 块控制台），CRSF 与配置那块都不在。应用是 `ZLRS`、bootloader 是 `ZLRS MCUBOOT`，脚本按 `/dev/serial/by-id/...` 找口。

**升级**：一次性烧录（BOOTSEL 写 bootloader + 首版应用）之后，后续固件更新**不需要再碰板子**——包括新固件起不来、USB 起不来、上位机没确认、镜像写坏这些情况。四条不变量：

1. **bootloader 不参与升级**（`0x000000` 那 128 KB 只有 BOOTSEL 能写）。它是固定的恢复入口；要换它就得按键。
2. **升级一律写非活跃槽**：`CONFIG_MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD=n`，由 MCUboot 挑槽，升级工具不指定槽。
3. **pending 由工具设，permanent 只由应用设**：镜像跑起来、上位机发来确认命令之后，应用才调 `boot_write_img_confirmed()`。
4. **任何"没确认"的路径都必须能自己复位**：应用活着但没确认 → 应用自己复位；应用挂死（哪怕在 USB 起来之前）→ bootloader 侧的看门狗复位。

升级时序（`scripts/zlrs-update.py` 一条命令走完）：

| # | 谁 | 做什么 |
| --- | --- | --- |
| 1 | 工具 | 应用配置口发"进恢复"命令（置 boot mode 标志） |
| 2 | 应用 | 应答并重启 |
| 3 | MCUboot | 进串口恢复，起 USB（两块 CDC） |
| 4 | 工具 | 上传镜像 → MCUboot 写**非活跃槽**并标 pending（test） |
| 5 | MCUboot | 复位 → 换槽（`swap using move`）→ 跑新镜像，此时仍是"未确认" |
| 6 | 工具 | 等新镜像配置口回来，比对 `image-hash` 是否就是刚上传的那一版 |
| 7 | 工具 → 应用 | 发 `confirm` 命令；应用据此调 `boot_write_img_confirmed()`，这一版转正 |
| 8 | 工具 | 报告"升级成功"以及设备报回的固件版本串（`build` 命令） |

失败模式：

| 失败 | 谁发现 | 自动动作 | 需要按键？ |
| --- | --- | --- | --- |
| 上传中断 / 镜像损坏 | MCUboot 校验 | 不换槽，停在串口恢复 | 否，重传 |
| 新镜像启动即挂死 | bootloader 看门狗（30 s） | 复位 → 未确认 → 回滚 | 否 |
| 新镜像跑起来但 USB 没枚举 | 应用确认超时（60 s） | 应用复位 → 回滚 | 否 |
| 新镜像跑起来、上位机没确认 | 应用确认超时 | 应用复位 → 回滚 | 否 |
| 工具第 6 步发现跑的不是新镜像 | 工具 | 不确认，等回滚，报"升级失败" | 否 |
| 两个槽都不可启动 | MCUboot | 停在串口恢复 | 否，工具重传 |
| bootloader 自己坏 | ROM | — | **是**（设计上接受） |

回滚的判据在 MCUboot 的镜像 trailer 里，应用侧只负责"该确认时才确认"：`boot_is_img_confirmed()` 为真就什么都不做，为假才等上位机（`ZLRS_APP_MCUBOOT_CONFIRM_TIMEOUT_MS`，默认 60 s）。用 BOOTSEL 直写进去的镜像 trailer 是空的，MCUboot 按"初始镜像"处理，不要求上位机确认——那是第一次烧录。

**看门狗**（`CONFIG_ZLRS_APP_WATCHDOG`，默认关）只负责"把卡死变成复位"：应用侧在 `PRE_KERNEL_1` 武装（排在熵源初始化之前，熵源 probe 挂了也复位得回来）、主循环与配置会话里喂；bootloader 侧由 `lib/bootguard`（`CONFIG_ZLRS_BOOTGUARD`）先清掉硬件 `ENABLE` 位（驱动的 `wdt_disable()` 在复位后的新一局里无效，否则串口恢复会被上轮看门狗打断），随后 MCUboot 用 `BOOT_WATCHDOG_TIMEOUT_MS`（30 s）重新武装，跳进应用后这一层还开着——应用连自己的看门狗都没武装就挂住时，靠它复位回滚。

一条命令升级：`python3 scripts/zlrs-update.py --port <应用配置口>`（触发进恢复 → 等恢复口 → 上传，由 MCUboot 自己写进**非活跃槽**并标成待试跑 → 等应用回来 → 发一条 `status` 确认）。工具不指定槽、也不写"永久"：镜像还没跑起来就标永久，等于自己把回滚这条路切掉。救砖走 BOOTSEL + 拖 UF2（见 [12-debug](12-debug.md)）。量产镜像用自己的签名密钥，换密钥要重烧 bootloader。

## 7. 错误处理约定

设备看起来在跑、实际某个端口早就没工作，是最贵的坑，所以：

| 层 | 能拿到什么 | 该做什么 |
| --- | --- | --- |
| `lib/`（纯 C，主机可测） | 只有输入输出，没有设备与日志 | 返回错误码/布尔值，**不打印**；密码学一律 fail closed |
| `app/src/`（Zephyr 胶水） | 设备、日志、策略 | 每个能判的返回值都要判，按后果选策略：退化（端口不可用 → 走另一块端点）、降级（NVS 不可用 → 不发射频）、致命（PSA 初始化失败 → `fault_loop()`） |
| `lib/usb`、`lib/bootguard` 等镜像级胶水 | 失败影响整机可用性 | 同样判，但默认不致命：USB 是可裁的，起不来就报警继续跑 |

具体规则：

- **返回值判到语义那一步**：`nvs_write()` 成功返回写入字节数、内容一致返回 0、出错返回负 errno——`rc == 0 || rc == len` 才算成功。
- **忽略返回值要写清理由**：只有 best-effort（透传/CRSF 发送，丢了有计数）与清理路径（`psa_destroy_key`、`wdt_disable`）。
- **API 返回 void 时要有别的机制暴露失败**：`uart_irq_rx_enable()` 判不了，就靠"回调是否装上"（`uart_irq_callback_user_data_set()` 返回 int，必须判）+ 上层超时/计数。
- **每帧都在跑的路径不许刷日志**：失败只报一次，或只更新计数器由状态命令呈现。
