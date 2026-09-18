# 路线图

本文只写"还没做什么、做完怎么算过"。设计已定稿的部分见各设计文档，这里不重复；每个里程碑的验收标准就是它的"完成"定义。

起点：协议核心、链路层（含 ARQ）、CRSF 端口、配置协议与地区配置层都已实现，并有主机侧双节点仿真覆盖；固件在 RP2350-Zero + Core2021 上可以构建、可以跑超帧，但**还没有上板验证**。

下面每个里程碑分两部分：**已做**（不依赖硬件、已经能在主机上验证的部分）与 **待验收**（必须上板或上台架才能算完成的部分）。

## M0 硬件定版

目标：把硬件定下来，后面所有测量都在它上面做。

- 硬件已定：微雪 RP2350-Zero（RP2350A，4 MB flash，板载 TRNG 供 CSPRNG 熵源）
+ 微雪 Core2021，板级定义在本仓库 `boards/zlrs/rp2350_zero_core2021/`
- sub-G 两个 SKU → 两个板级变体：`.../m33/hf`（868 / 915）与 `.../m33/lf`（470 / 433）；两套 PA 标定表与标定频点独立，地区与 SKU 不匹配时固件拒发
- 板级标定：按所选模块的 PA 网络重标 `tx-power-cfg-*` 与 `calibration-freqs`
- 接线核对：射频 SPI0 与 BUSY/NRESET/DIO8、数据 UART1、按键按板级定义接上，空闲引脚留给就地调试
- 验收：模块在目标频段上收到已知发射源的 RSSI 与 datasheet 量级一致

## M1 上板起链

目标：两块板子跑通"配对 → 会话 → 透传"。

- 待验收：全部。这一条没有任何"先在主机上做"的空间
- 已做（上板验证，"除射频外全开"那一档）：灯带/状态灯（紫=未配置根密钥 → 写完短语后按文档变绿）、PM_DEVICE、SPI + USP/LBM 都跑得起来；三块 CDC、配置模式（数据口魔数 `zlrs!configmode!` 与 shell 的 `zlrs config` 两个不依赖按键的入口）、配置协议、NVS 持久化、shell（status/ports/config/recovery）全部通过。**按钮-free 升级链路也验证了**：数据口魔数/SHELL 触发 → MCUboot 串口恢复 → `smpmgr` 上传 → 应用自确认 → 起来，全程不用按 BOOTSEL。唯一没开的是 **RP2350 TRNG**（见下面的"待修"）
- 已做（软件侧）：上板调试的分层开关——不带 tag（P0）、不跳频、不握手、不发遥测都能单独关，关掉后链路照常工作且有主机侧用例；**整块射频也能关**（`CONFIG_ZLRS_APP_RADIO=n`，走 `app/src/radio_null.c`）：Core2021 还没接上时先验证端口、配置模式、按键、状态灯与 NVS，见 [01](01-system.md) 第 2.3 节
- 已做（软件侧，上板发现并修）：**USB 设备栈由镜像自己建**（`lib/usb`，应用侧 `CONFIG_ZLRS_APP_USB`）。Zephyr 的 `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` 只注册第一块 CDC，而本板应用三块、bootloader 两块——没注册的那块没有 context，第一次往它写日志就是 NULL 解引用、整机停住（上板第一次跑就是这个现象：USB 直接掉线）。现在应用与 bootloader 各自建 context、注册 devicetree 里全部 CDC。上板已验证：bootloader 枚举出两块 CDC、应用出三块。见 [01](01-system.md) 第 2.2 节
- 已做（软件侧，上板发现并补）：**"起来就崩"的镜像别再要 BOOTSEL**——镜像确认位（`CONFIG_ZLRS_APP_MCUBOOT_CONFIRM`：初始化跑完调 `boot_write_img_confirmed()`）配上 MCUboot 的 test/revert：没确认的新镜像崩掉、复位后自动回滚；没有上一版就停在串口恢复。另外预埋了 `CONFIG_ZLRS_APP_WATCHDOG`（默认关，调试期开），把"崩溃停在原地"变成"超时复位"，这样回滚才真的会发生。见 [01](01-system.md) 第 2.4 节
- 已修（上板发现，第一次上板的真正拦路虎）：**LF 板的 SKU 声明读不到**。`zlrs,subg-band` 原来写在 `/chosen` 里，而 Zephyr 的 DT 宏只导出 `zephyr,` 前缀的 chosen 属性——自定义前缀静默读不到，`ZLRS_SUBG_BAND` 永远退回默认值 HF，于是 LF 板跑 CN470/CN433 必然被 `zlrs_region_validate()` 拦下并 `fault_loop()`：现象是"应用起来就停住、USB 半死、灯也不亮"，看着像驱动或 USB 的问题，其实跟射频一点关系没有。现在 SKU 是独立 DT 节点（`zlrs,subg-band` + `dts/bindings/zlrs,subg-band.yaml`，`app/src/board.h` 从节点读），上板已验证：LF 板 + CN433 正常进超帧循环
- 已修（上板发现）：**三块 CDC 的接收端装死**。全速设备上 `CONFIG_USBD_CDC_ACM_BUF_POOL` 默认关，三块 CDC 都去抢 UDC 那个 `UDC_BUF_POOL_SIZE=1024` 的公共池；枚举刚完成时池子还被控制传输与控制台 TX 占着，CDC 的 RX 工作项（`rx_fifo_work`）一旦 `cdc_acm_buf_alloc()` 失败就直接 return，而 `CDC_ACM_RX_FIFO_BUSY` 位不清——之后永不重试，**接收端从此收不到任何字节**（TX 照常，因为 TX 是发送时才分配）。更坑的是 CDC 驱动自己的日志因为"控制台挂在 CDC 上"被编译期强制成 `LOG_LEVEL_NONE`，所以毫无提示：数据口魔数、配置工具、CRSF 输入全都"发出去没反应"。修法是让每块 CDC 用自己的池（`CONFIG_USBD_CDC_ACM_BUF_POOL=y`，见 app/prj.conf 的注释）
- 待查（上板发现）：**换槽（`BOOT_SWAP_USING_OFFSET`）这条路还没走通**。用 test 语义（`smpmgr upgrade`，不带 `--confirm`）刷过一次之后，设备停在"USB 描述符请求失败"——芯片在跑但控制传输没人应答，只能按 BOOTSEL 捞回来。怀疑是换槽写 flash 期间 XIP 取指/中断被打断（也可能是别的），要上 SWD 看现场。**调试期先用 `--confirm`**（`scripts/zlrs-update.py --confirm`）：直接写 slot0、标记永久，不走换槽，代价是没有回滚兜底
- 已修（上板发现）：**看门狗把串口恢复打断了**。RP2350 的看门狗在 `SYSRESETREQ` 之后还在跑（`ENABLE` 位不清），于是应用重启进恢复时，上轮点着的 8 秒看门狗跟着进了 bootloader，MCUboot 的串口恢复**只活 ~1.5 s** 就被复位——主机侧现象是"恢复口闪现一下、镜像永远传不完"。两边都补了：应用在重启前 `zlrs_watchdog_disarm()`（应用侧驱动状态是 enabled，`wdt_disable()` 有效）；bootloader 护栏改成直接清硬件 `ENABLE` 位（驱动的 `wdt_disable()` 在复位后的新一局里会直接 `-EFAULT` 返回、什么都不做）。见 [01](01-system.md) 2.4
- 已修（上板发现，非射频那一档的三个拦路虎）：① **`nvs_write()` 成功时返回写入字节数**，不是 0——`write_record()` 直接透传返回值、上层按 `!= 0` 判错，于是每次写 NVS 工具都报 `storage error`（数据其实写进去了）；② **配置模式用 `uart_poll_in/out`**，而数据口的 ISR 同时在搬字节，`poll_in` 永远读到空——能进配置模式但一条命令都不应答；③ **写短语要 100000 轮 PBKDF2**（M33 上十几秒），8 秒看门狗会把配置会话复位掉——会话开始先 `zlrs_watchdog_disarm()`、结束再武装。三条都在 `77b36b1`，上板自检（`scripts/zlrs-bringup.py`：端口 → 魔数进配置 → 协议读写 → NVS 持久化）已全绿
- 待修（上板发现，**RP2350 的 TRNG 会让 USB 死掉**）：打开 `CONFIG_ENTROPY_RPI_PICO_RNG`（SoC 默认 y）之后，应用照常跑、状态灯照常闪，但主机侧"设备描述符请求失败"——数据口、控制台、升级全断。根因在 pico-sdk 的取数路径（`modules/hal/rpi_pico/src/rp2_common/pico_rand/rand.c`）：TRNG 采样是"拿住自旋锁 `spin_lock_blocking` + 关中断 + `while (trng_hw->trng_busy);`"的忙等，USB 的中断与工作队列在这期间得不到服务。**当前对策**：默认关（`app/prj.conf` 的 `CONFIG_ENTROPY_RPI_PICO_RNG=n`，用 `ZLRS_APP_ENTROPY_TRNG` 才打开），CSPRNG 退到 Zephyr 的默认路径——bring-up 够用，**量产前必须解决**：要么给 TRNG 写一个"有界超时 + 不长时间关中断"的取数路径，要么换别的熵源
- 已做（软件侧）：**MCUboot 启动链与两槽分区**——`.../m33/{hf,lf}/mcuboot` 变体（sysbuild），镜像 ECDSA-P256 签名、`swap using offset` 可回滚、bootloader 串口恢复走数据口 UART1；第一次烧写拖两个 UF2 即可，之后升级不用按键。见 [01](01-system.md) 第 2.4 节
- 已做（软件侧，升级入口）：**恢复只走 USB CDC**——bootloader 模式下 USB 留两块 CDC（第 1 块恢复、第 2 块内核日志），CRSF 那块由 per-image overlay 裁掉；插板载 USB-C 就能升级，不需要 UART 转接。见 [01](01-system.md) 2.4
- 已做（软件侧，升级入口）：**应用侧触发进串口恢复**——数据口收到 16 字节魔数（`scripts/zlrs-recovery.py`）就置 boot mode 并重启，MCUboot 停在串口恢复等上传；升级一台健康设备不用拔插、不用按键。标志放在 SRAM 起始的 4 字节 retained 区（Zephyr 树里 RP2350 专用覆盖），应用与 bootloader 共用同一份 DT。见 [01](01-system.md) 2.4
- 已做（软件侧，主机侧一条命令）：**`scripts/zlrs-update.py`**——触发进恢复 → 按 USB 名字等 bootloader 的恢复口 → `smpmgr upgrade` 上传签名镜像并重启 → 等应用端口回来。两种模式在 USB 上分别自报 `ZLRS` 与 `ZLRS MCUBOOT`，脚本据此找端口；`--client none` 只打印命令，`--no-trigger` 用于已在恢复模式的设备，`--self-test` 不接设备
- 待做（升级链路的后续，按收益排序）：① 应用侧 mcumgr（真正的运行中升级：镜像直接写 slot1，连"重启进 bootloader"都省了；需要把 SMP 流和透传流在数据口上复用）；② 无线 OTA（镜像走 2.4G 透传链路，≈10 s/1 MB，见 M4）
- 验收：150 Hz 控制帧率下连续 10 分钟不掉线；透传 200 kB，串口侧丢弃计数与链路统计能对上（best-effort 允许丢，但必须看得见）
- 测量：射频回调延迟抖动、频段切换开销、各速率档的误包率曲线、实际透传吞吐
- 调参：接收窗口长度、超帧预算、tick 粒度（当前量化到 1 ms，见 01 第 7 节）

## M2 控制链路可用

目标：接上遥控器与飞控就能飞。

- 已做：CRSF 编解码（`lib/crsf`）、TX 侧输入解析、RX 侧输出重建、失控保护（TX 输入超时与 RX 未连接都会输出预设值）、TLM 组装（上行数字来自 RX 的遥测回报，下行数字由 TX 自己测），端口挂在 `zlrs-crsf-uart`（UART0）与 `zlrs-crsf-cdc`（USB）两个别名上
- 待验收：拨杆到舵机的实际延迟、拔掉 TX 电源后 RX 的失控保护时间（目标 100 ms）
- 验收：拨杆到舵机的端到端延迟与丢包可量化；拔掉 TX 电源，RX 在 100 ms 内进入失控保护

## M3 配置与身份

目标：不用改固件就能配好一台设备。

- 已做：配置模式（开机按住按键进入，数据口轮询跑配置协议，无操作 300 秒或 EXIT 退出）、命令集（读状态、触发配对、写根密钥、写短语、写失控值、改波特率）、短语 → PBKDF2 → 根密钥（salt 与迭代次数固定在设备侧）落 NVS
- 已做（上板验证，软件侧能验的部分）：**非射频自检全绿**（`python3 scripts/zlrs-bringup.py`）——三块 CDC 都在、数据口魔数进配置模式、`status`/`set-phrase`（PBKDF2 落 NVS）/`set-failsafe`/`exit` 全部 ack ok、退出后再进配置模式 `key=yes` 仍在（NVS 持久化）。板上没有用户按键，入口用 `CONFIG_ZLRS_APP_CONFIG_MAGIC` 的数据口魔数（也可以用 `CONFIG_ZLRS_APP_SHELL` 的 `zlrs config`）
- 待验收：拿一台空片 RX，只用配置工具（`scripts/zlrs-config.py`）与一根串口线配到能配对
- 验收：一台空片 RX 到能配对，只用配置工具与一根串口线

## M4 数据链路达标

目标：透传接近空口上限，且行为可预期。

- 已做：ARQ profile（累计 ACK、窗口 4 帧、超时重传、重传用尽后重同步）、背压计数（队列满丢掉的字节、ARQ 放弃的段）、速率自适应框架（档位表 + 连续窗口 + 保持时间 + 滞回）、动态功率（按 RX 遥测回报的 RSSI/LQ 升降 TX 功率，遥测断了回到上限；见 03 的 4.6）
- 待标定：速率档的阈值与滞回参数必须按实测误包率曲线填（框架与阈值是分开的）动态功率的 RSSI/LQ 阈值同理（`lib/link/power.c` 里是占位值）
- 已做（软件侧）：**调制按链路各自选**。① `zlrs_radio_req_t` 与链路配置换成 `zlrs_mod_cfg_t`（调制 + 参数），移植层按 `modulation` 填 RAC 的 `radio_params.lora` / `.flrc`；② 速率档表与空中时间模型落到 `lib/proto/mod.c`（FLRC 沿用原模型，LoRa 按 Semtech 公式），超帧时隙、ARQ 超时由档位派生；③ LoRa 的同步字另派生 1 字节 + private bit，配对帧固定落在私网同步字上。见 [03](03-protocol.md) 4.1.1
- 待验收（LoRa 远距档）：上板量误包率与灵敏度、核 SF7/BW125 的实测空中时间与模型差多少、EU868 的 1% 占空比下这条路是否还能用（结论会写回 [03](03-protocol.md) 4.1.1）
- 已做（路径 C）：**控制帧率档 150 / 500 Hz**。帧率、时隙预算与遥测/数据窗口的稀疏系数由 `zlrs_region_control_plan()` 按地区占空比与空中时间算出；500 Hz 档在 EU868 会被压到 ~46 Hz（1% 占空比），窄带速率下会被压到 ~470 Hz
- 已做（路径 A 的协议侧）：**1000 Hz 的时间计划与协议时序**。1 ms 一帧的时长预算（31 B 帧 108 µs、时隙 308 µs）、遥测/数据的稀疏系数（1/6、1/10）、超帧与跳频节奏都有主机侧用例（`region: 1000 Hz plan`、`link: 1000 Hz frame cadence`）
- 待做（路径 A 的射频侧，**需要上板**）：控制链路自己拥有射频——绕开 RAC 的毫秒级 planner，直调 LR20xx 驱动（`rac/direct_driver_access` 展示的就是这条路径：`smtc_rac_lock_radio_access` 拿住射频，在锁窗口里用 IRQ 驱动逐帧收发）。为什么放到上板之后：① 直调要自己复现板级的 PA 配置与功率表（`ral_lr20xx_bsp_*` 那套按 SKU 与 dtsi 标定表生成，写错了轻则功率不对、重则伤 PA）；② DIO/IRQ 时序、TX→RX 翻转开销只能实测；③ 直调期间数据链路的时间片要自己排（或加第二颗射频），配合 `ZLRS_APP_LINK_DATA=n` 先跑单链路。协议层（帧/标签/LQ/状态机）不用动——`zlrs_radio_ops_t` 就是换后端的接缝
- 另一个候选（不解决 1000 Hz，但值得试）：**burst 给数据链路用**——2.4G 批量流，把"每帧一次事务"换成"一次事务一片数据"，省掉每事务开销。待上板验证
- 待验收：配置的串口波特率下，持续吞吐与延迟达到 01 第 4 节的估算量级
- 验收：配置的串口波特率下，持续吞吐与延迟达到 01 第 4 节的估算量级

## M5 形态完整

- 已做：地区配置层（三张表、启动校验、发射前占空比记账）；CRSF 与数据口都挂在两块端点上（UART0/1 + USB CDC，CDC 被主机打开就走 USB，否则走 UART）；USB 三个 CDC——CRSF、数据、控制台（`zephyr,console`）
- 验收：USB 版与轻量版共用同一份固件，差异全部来自配置

## M6 抗干扰

- 已做（软件侧）：连接态的周期性重同步与漂移处理——每 `CONFIG_ZLRS_APP_RESYNC_PERIOD_MS`（默认 1 s）发一条 `BEACON`，RX 据此把跳频索引拉回来；同一条路也解决了"对端重启后自动重连"（见 03 第 5.1 节）。用例：`link: hop resync`、`link: peer restart reconnect`
- 待验收：逐包跳频上板验证——时隙锁步、真实晶振偏差下长时间飞行的漂移速率、重同步周期该取多大
- 共存与干扰测试：同频段多套设备、单音与宽带干扰
- 法规参数核对：FCC 跳频条款与 ETSI 占空比
- 验收：干扰下的链路保持率与恢复时间可量化

## 长期方向

频段角色可配（竞速模式：控制链路独占 2.4G，见 01 第 3.2 节）——已做（软件侧，`CONFIG_ZLRS_APP_BAND_ROLE_*`）：模式切换、法规门只作用于 sub-G 那条链路、控制帧率与数据窗口的摊分都有主机侧用例（`region: racing plan`）；待上板验证 2.4G 独占下的实际延迟与 sub-G 数据链路的吞吐。
