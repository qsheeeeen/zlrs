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

量产固件里没有 console CDC、没有 shell、没有魔数入口；三块 CDC 是 CRSF / 数据 / 配置（见 [08-firmware](08-firmware.md) 第 1 节）。

## 2. 临时入口

这些只存在于调试构建，正式方案见 [10-security](10-security.md) 第 10 节与 [08-firmware](08-firmware.md) 第 2 节：

| 入口 | 用法 |
| --- | --- |
| 数据口魔数 `zlrs!configmode!` | 开关 `CONFIG_ZLRS_APP_CONFIG_MAGIC`；数据口收到 16 字节就进配置模式（与 `zlrs!bootloader!` 共用前缀匹配，正常字节不丢） |
| 数据口魔数 `zlrs!bootloader!` | 开关 `CONFIG_ZLRS_APP_UPDATE_TRIGGER`；重启进 MCUboot 串口恢复（`scripts/zlrs-recovery.py`） |
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
- **看门狗**在调试期建议打开（`CONFIG_ZLRS_APP_WATCHDOG=y`）：刷坏的镜像会自己复位，配合 MCUboot 的 test/revert 自动回滚，停在串口恢复里，不用按 BOOTSEL。
- **恢复口按名字找**：应用是 `ZLRS`、bootloader 是 `ZLRS MCUBOOT`，用 `/dev/serial/by-id/...` 而不是 `ttyACM*`。
- **不要用 `cat` 读 console**：会把 tty 回显打开，设备日志被回送成 shell 输入。
- **USB/IP 转发**（usbipd）下 CDC 的 DTR 可能送不到，端点选择依赖"收到过主机字节"，排查端点问题时用 `zlrs ports` 看两块端点的 `ready/active/rx_total`。
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
