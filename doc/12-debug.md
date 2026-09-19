# 开发与调试

本文只给开发者看：调试构建怎么开、分层调试开关怎么用、临时入口有哪些、上板怎么定位问题。用户文档不出现这些内容（见 [README](README.md) 的写作约定）。

## 1. debug overlay

调试构建 = 正常构建 + 两个文件：

| 文件 | 内容 |
| --- | --- |
| `app/debug.conf` | 打开 console CDC、shell、看门狗、调试开关（见第 3 节） |
| `app/debug.overlay` | devicetree 里加一块 console CDC（`zephyr,console` / `zephyr,shell-uart`），并把调试入口所需的节点补齐 |

构建时挂上去：

```shell
west build -b "$ZLRS_BOARD" "$ZLRS_APP" -- \
  -DEXTRA_CONF_FILE=debug.conf -DDTC_OVERLAY_FILE=debug.overlay
```

量产固件里没有 console CDC，也没有 shell；三块 CDC 是 CRSF / 数据 / 配置（见 [08-firmware](08-firmware.md) 第 1 节）。

## 2. 调试入口

这些只存在于调试构建，正式方案见 [10-security](10-security.md) 第 10 节与 [08-firmware](08-firmware.md) 第 2 节：

| 入口 | 用法 |
| --- | --- |
| shell `zlrs config` / `status` / `ports` / `recovery` | 开关 `CONFIG_ZLRS_APP_SHELL`；在 console CDC 上执行 |

## 3. 分层调试开关

第一次上电不要一次把所有东西都打开：每关掉一层，剩下的还能跑通就说明问题不在被关掉的那一层。关掉的项在开机时会打一条 `bring-up:` 警告。每个开关都有对应的主机侧用例（`tests/host`）。

| 开关 | 关掉之后 | 主机侧用例 |
| --- | --- | --- |
| `CONFIG_ZLRS_APP_SECURITY_PROFILE_P0=y` | 帧不带 tag，先跑通收发时序与帧格式 | `link: bring-up without tag (P0)` |
| `CONFIG_ZLRS_APP_FHSS=n` | 固定发在同步信道 | `link: bring-up without hopping` |
| `CONFIG_ZLRS_APP_SESSION_HANDSHAKE=n` | 配对级钥匙直接当会话钥匙 | `link: bring-up without session handshake` |
| `CONFIG_ZLRS_APP_TELEMETRY=n` | 控制链路只剩下行 RC（TX 的失联判定一并关掉） | `link: bring-up without telemetry` |
| `CONFIG_ZLRS_APP_DATA_ARQ=n` | 数传回到 best-effort | `link: arq *`、`link: backpressure` |
| `CONFIG_ZLRS_APP_RATE_ADAPT=n` | 固定速率档 | `link: rate adapt` |
| `CONFIG_ZLRS_APP_DYNAMIC_POWER=n` | 固定发射功率 | `power: *` |
| `CONFIG_ZLRS_APP_LINK_CONTROL=n` / `ZLRS_APP_LINK_DATA=n` | 只跑一条链路 | 编译验证（两个 SKU 都在 CI 里） |
| `CONFIG_ZLRS_APP_RADIO=n` | 射频整块不参与（`app/src/radio_null.c`）：不初始化 USP/RAC、不提交事务，每次事务当场按"发出去、没收到回音"收尾；端口、配置、按键、状态灯、NVS 照常 | 编译验证 + 上板（USB/端口/配置） |
| `CONFIG_ZLRS_APP_CRSF_PORT=n` | 不初始化 CRSF 端口：TX 只发配置好的失控值 | 编译验证 |
| `CONFIG_ZLRS_APP_STATUS_LED=n` | 关状态灯 | 编译验证 |

推荐顺序：`P0` + 关跳频 + 关握手 + 关遥测 → 确认"自己发的帧自己能收到"；然后打开 tag（`P1`）；再打开跳频；再打开遥测与失联判定；最后才是速率自适应与动态功率——这两项的阈值要等实测曲线。

## 4. 上板调试方法

- **先关射频**（`ZLRS_APP_RADIO=n`）验证 USB、端口、配置模式与 NVS：`python3 scripts/zlrs-bringup.py` 一条命令跑完（三块 CDC → 配置模式 → 配置协议读写 → NVS 持久化）。
  模块没接时用现成的配置文件：

  ```shell
  west build -b "$ZLRS_BOARD" "$ZLRS_APP" -- \
    -DEXTRA_CONF_FILE="$ZLRS_REPO_ROOT/app/debug.conf;$ZLRS_REPO_ROOT/app/bringup-noradio.conf" \
    -DDTC_OVERLAY_FILE="$ZLRS_REPO_ROOT/app/debug.overlay"
  ```

  **射频打开而 Core2021 没接会黑灯**：vendored 的 `lr20xx_hal_wait_on_busy()` 在等 BUSY 的死循环里忙等（`k_usleep` 被注释掉了），默认超时 600 s，超时还 `k_oops()`；它跑在主线程（最高优先级），USB 栈拿不到 CPU，表现是"灯不亮 + USB 完全不枚举 + 应用被 8 s 看门狗反复复位"。这与应用代码无关，模块接上就正常。
- **看门狗**在调试期建议打开（`CONFIG_ZLRS_APP_WATCHDOG=y`）：刷坏的镜像会自己复位，配合 MCUboot 的 test/revert 自动回滚，停在串口恢复里，不用按 BOOTSEL。
- **恢复口按名字找**：应用是 `ZLRS`、bootloader 是 `ZLRS MCUBOOT`，用 `/dev/serial/by-id/...` 而不是 `ttyACM*`。
- **判断"射频到底发出去了没有"，看每秒状态日志的 `air:` 那一行**（`CONFIG_ZLRS_APP_STATS_LOG=y`）：`ctl_tx` / `data_tx` 只统计事务真的做完的帧，`fail` 是事务失败次数。只有一块板时唯一能上空的路径是配对——配置口 `pair` 之后 30 s 窗口里每 100 ms 一个 `PAIR_REQ`，`ctl_tx` 该涨到 300 上下，`fail` 保持 0。
- **不要用 `cat` 读 console**：读一小会儿就收不到东西了，而且会把 tty 回显打开、设备日志被回送成 shell 输入。用 pyserial 那种一直读的方式（`scripts/` 之外的自备小工具就行）。
- **USB/IP 转发**（usbipd）下 CDC 的 DTR 可能送不到，端点选择依赖"收到过主机字节"，排查端点问题时用 `zlrs ports` 看两块端点的 `ready/active/rx_total`。
- **usbipd 转发下设备重新枚举会整个从 WSL 掉口**（`/dev/serial/by-id/` 都消失）：应用复位进 bootloader、或换槽之后重启，都会碰到。在 Windows 侧重挂（detach → attach 到 WSL）再接着干；升级工具此时会停在"等恢复口/等应用口"，设备回来以后用 `--recovery-port <恢复口> --no-trigger` 继续，不必从头再触发一遍。
- **射频相关的量**（回调延迟、换频开销、误包率、吞吐）只能在板上量，可以直接用 Semtech 官方 `packet_error_rate_flrc` 示例做对照。

## 5. 踩过的坑（保留一行摘要，细节在代码注释里）

- 全速设备上多块 CDC 抢 UDC 公共池，`cdc_acm_buf_alloc()` 失败后 RX 静默失效（`CONFIG_USBD_CDC_ACM_BUF_POOL=y`）。
- `CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` 只注册第一块 CDC → 未注册的那块写日志会 NULL 解引用（USB 栈自建）。
- `nvs_write()` 返回值判错 → 把成功当失败（按 `rc == 0 || rc == len` 判）。
- 配置模式用 `uart_poll_in/out` → 一条命令都不应答（走端口层的 ISR 缓冲）。
- 写短语的 PBKDF2 要几十秒 → 会话期间必须关看门狗。
- RP2350 的看门狗在 `SYSRESETREQ` 后还开着 → 串口恢复只活 ~1.5 s（应用侧重启前 disarm，bootloader 侧清硬件 `ENABLE` 位）。
- 自定义 DT 节点写进 `/chosen` 读不到（只导出 `zephyr,` 前缀）→ SKU 用独立节点。
- RP2350 TRNG 取数时长时间关中断 → USB 死掉（默认关，见 [13-roadmap](13-roadmap.md)）。
- 射频事务提交成功却一个包都发不出去：RAC 的 planner 要有人在主循环里推 `smtc_rac_run_engine()`，漏了就永远停在 `RP_STATUS_TASK_INIT`，而且**不报错**——现象是 `air: ctl_tx=0`、`fail=0`。官方示例的主循环里都有这一句。
- 配置口触发配对后配对窗口立刻过期：主循环的 `now_ms` 是在进配置会话**之前**取的，会话里发生的事（配对按键）用的时钟比它晚，`now_ms - state_ms` 是负的、无符号相减直接下溢 → 第一次 tick 就判成窗口到期。会话结束后重新取一次时钟。
- 给 bootloader 把恢复窗口拉长（`app/sysbuild/mcuboot-debug.conf`）之后，主机先枚举了 bootloader 的 USB，应用再接管时主机不会重新枚举——应用起不来，看门狗复位，看起来像"应用坏了"。正常构建的 2 s 窗口碰不到；应用侧另外在 `lib/usb/usb_dev.c` 里把 USB 控制器按住复位 100 ms，逼主机重新枚举。
