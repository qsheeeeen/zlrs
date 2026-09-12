# 线程模型与 RAC 使用方式

本文结论来自阅读 `usp_zephyr` v1.0.0 的 `doc/THREAD_MANAGEMENT.md`、
`subsys/usp/zephyr_usp_thread.c`、`include/zephyr/usp/smtc_sw_platform_helper.h`，
以及 `usp` 仓库里的 RAC 示例（`ping_pong`、`packet_error_rate_flrc`）。

## 1. USP 提供的三种线程模式

| 模式 | Kconfig | RAC 线程优先级 | 主线程优先级 | API 调用方式 |
| --- | --- | --- | --- | --- |
| 单线程 | `CONFIG_USP_MAIN_THREAD=n` | 无 | — | 直调 |
| 协作式多线程 | `=y`，`USP_THREADS_MUTEXES=n` | `-4` | `-2` | 直调 |
| 抢占式 + 互斥锁 | `=y`，`USP_THREADS_MUTEXES=y` | `1` | `3` | `SMTC_SW_PLATFORM()` 加锁 |

`SMTC_SW_PLATFORM(call)` 的实际展开（`smtc_sw_platform_helper.h`）：

```c
/* 单线程，或协作式多线程：直调 */
#define SMTC_SW_PLATFORM(call) call

/* 抢占式 + 互斥锁：加锁 */
#define SMTC_SW_PLATFORM(call)                        \
    ({ k_mutex_lock(&rac_api_mutex, K_FOREVER);        \
       __auto_type __result = (call);                  \
       k_mutex_unlock(&rac_api_mutex);                 \
       __result; })
```

协作式模式下不需要互斥锁，是因为线程只在让出点切换，临界区天然不会被中断。

## 2. RAC 的执行模型

不启用 LBM 时，USP 线程的主循环是（`zephyr_usp_thread.c`）：

```c
sleep_time_ms = CONFIG_USP_MAIN_THREAD_MAX_SLEEP_MS;
smtc_rac_run_engine();
if (smtc_rac_is_irq_flag_pending()) continue;   /* 有射频中断就立刻再跑 */
smtc_modem_hal_interruptible_msleep(K_MSEC(sleep_time_ms));
```

也就是说：**射频中断唤醒 RAC 线程 → 跑 `smtc_rac_run_engine()` → 派发回调 →
没事就睡**。不装 LBM 时，这个线程基本只为我们的链路服务。

一次事务的生命周期（照抄官方示例的用法）：

```c
/* 初始化：一条"链路"对应一个 radio_access_id */
id = SMTC_SW_PLATFORM(smtc_rac_open_radio(RAC_HIGH_PRIORITY));
tx = smtc_rac_get_context(id);
tx->modulation_type = SMTC_RAC_MODULATION_FLRC;
tx->scheduler_config.scheduling = SMTC_RAC_SCHEDULED_TRANSACTION;
tx->scheduler_config.callback_pre_radio_transaction  = pre_cb;
tx->scheduler_config.callback_post_radio_transaction = post_cb;
tx->smtc_rac_data_buffer_setup.tx_payload_buffer = ...;
tx->radio_params.flrc.tx_size = ...;

/* 每次收发：填参数 -> 提交 */
SMTC_SW_PLATFORM(smtc_rac_submit_radio_transaction(id));
```

关键约束：

- `smtc_rac_get_context()` 拿到的上下文是**共享、可复用**的，示例都是初始化时
  一次性配好，之后每个事务只改长度/频率等字段。
- 提交是异步的，结果通过 `callback_post_radio_transaction(rp_status_t)` 返回。
- **回调在 RAC 线程上下文里执行**，就在 `smtc_rac_run_engine()` 内部。

### 可用的优先级

`radio_planner_hook_id_defs.h` 定义了三个 RAC 档位：

```
RP_HOOK_RAC_VERY_HIGH_PRIORITY
RP_HOOK_RAC_HIGH_PRIORITY
RP_HOOK_RAC_MEDIUM_PRIORITY
```

这就是我们分时双频的仲裁手段，不用自己造调度器。

## 3. 一个必须先说清楚的坑

官方示例（`ping_pong`、`packet_error_rate_flrc`）**在 post 回调里直接提交下一个
事务**。这个写法只在 `SMTC_SW_PLATFORM` 是直调时安全，因为：

- 抢占式 + 互斥锁模式下，`smtc_rac_run_engine()` 是在持有 `rac_api_mutex`
  的情况下跑的；
- 回调里再走一遍 `SMTC_SW_PLATFORM(smtc_rac_submit_radio_transaction(...))`，
  就是对同一把锁再次加锁。

Zephyr 的 `k_mutex` 对同一线程应该是可重入的（内部有 lock_count），所以大概率
不会死锁——但这条路径官方示例没覆盖，**要在选定的配置上实测确认**。

结论无论哪种模式都成立：**回调里只做搬运，不做决策**。

## 4. zlrs 的选择

### 起步用协作式

理由：

1. 这是官方示例实际验证过的配置，`prj.conf` 可以直接对齐。
2. API 直调，没有互斥锁开销，也没有优先级反转这一类问题。
3. 我们的链路逻辑全是 I/O 驱动、会频繁让出，不长时间占 CPU。

代价是：UART 突发如果处理太久，会推迟射频事件的处理。缓解办法是把串口处理
切块并及时让出。**这是需要在真板上量化的第一件事**，量出来不够再切抢占式。

### 线程划分

| 线程 | 优先级 | 职责 |
| --- | --- | --- |
| RAC 线程（USP 提供） | `-4` | 跑 radio planner，派发收发回调 |
| 链路线程（我们的 `main`） | `-2` | 链路状态机、组装/提交事务、结果处理 |
| 串口/USB 线程 | `>= 0` | 环形缓冲收发，不做任何射频相关决策 |

### 两条链路 = 两个 RAC 客户端

| 链路 | RAC 优先级 | 频段 | 调制 |
| --- | --- | --- | --- |
| 控制链路 | `RAC_HIGH_PRIORITY` | sub-GHz | FLRC（LoRa 兜底档） |
| 数据链路 | `RAC_MEDIUM_PRIORITY` | 2.4GHz | FLRC |

数据是**单颗半双工射频**，同一时刻只能有一个事务在跑；优先级决定谁先拿到射频，
这正好实现"控制链路永远优先，空闲时隙给透传"。

注意 RAC 是在 1.6 GHz 处分流收发通道的
（`ral_lr20xx_bsp_get_rx_cfg()`：`freq >= 1.6e9` 走 HF），所以我们只管填频率，
不用手动切路径。

### 回调契约

`callback_post_radio_transaction` 里只允许：

1. 记录 `rp_status_t` 与收到的长度；
2. 把结果投递给链路线程（`k_msgq` / `k_sem`）；
3. 返回。

不在回调里做 CRC 校验、不在回调里翻协议栈、不在回调里碰 UART。

## 5. 待验证清单

1. 协作式下，串口满载时射频回调的延迟抖动有多大。
2. 上面那条"回调用 `SMTC_SW_PLATFORM` 提交"在互斥锁模式下是否真的不死锁。
3. 两个 `radio_access_id` 的实际抢占行为：控制链路提交时，数据链路正在进行
   的事务是被打断还是被排在后面。
4. `SMTC_RAC_SCHEDULED_TRANSACTION` 的 delay 单位与精度（示例里是 ms 量级），
   能否支撑我们需要的时隙精度。
