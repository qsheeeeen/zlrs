/* SPDX-License-Identifier: Apache-2.0 */
#include <hardware/gpio.h>
#include <hardware/structs/io_bank0.h>
#include <hardware/structs/uart.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

#include "serial.h"

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(zlrs_data_uart));
static uint8_t bytes[256];
static atomic_t work_ran;

static zlrs_serial_state_t state(void)
{
	zlrs_serial_state_t s;
	zassert_equal(zlrs_serial_state(&s, 1), 1);
	return s;
}

static void allow_tx(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	gpio_set_inover(10, GPIO_OVERRIDE_LOW);
}

K_TIMER_DEFINE(cts_rescue, allow_tx, NULL);

static void mark_work(struct k_work *work)
{
	ARG_UNUSED(work);
	atomic_set(&work_ran, 1);
}

K_WORK_DEFINE(marker, mark_work);

static void *setup(void)
{
	const uint32_t old_ibrd = uart1_hw->ibrd;
	const uint32_t old_fbrd = uart1_hw->fbrd;
	zassert_ok(zlrs_serial_init(230400));
	struct uart_config cfg;
	zassert_ok(uart_config_get(uart, &cfg));
	zassert_equal(cfg.baudrate, 230400);
	zassert_equal(cfg.flow_ctrl, UART_CFG_FLOW_CTRL_RTS_CTS);
	zassert_true(uart1_hw->ibrd != old_ibrd || uart1_hw->fbrd != old_fbrd);
	zassert_equal(uart1_hw->cr & (UART_UARTCR_RTSEN_BITS | UART_UARTCR_CTSEN_BITS),
		      UART_UARTCR_RTSEN_BITS | UART_UARTCR_CTSEN_BITS);
	zassert_equal(gpio_get_function(10), GPIO_FUNC_UART);
	zassert_equal(gpio_get_function(11), GPIO_FUNC_UART);
	printk("UART baud=%u RTS/CTS enabled; hardware divider updated\n", cfg.baudrate);
	return NULL;
}

static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);
	k_timer_stop(&cts_rescue);
	hw_clear_bits(&uart1_hw->cr, UART_UARTCR_LBE_BITS);
	gpio_set_inover(10, GPIO_OVERRIDE_LOW);
	/* Also recover a failed backpressure assertion before the next test. */
	for (int i = 0; i < 200; i++) {
		(void)zlrs_serial_read(bytes, sizeof(bytes));
		k_msleep(1);
	}
	gpio_set_inover(10, GPIO_OVERRIDE_NORMAL);
	(void)uart_err_check(uart);
}

ZTEST(serial_hw, test_cts_blocks_tx_without_blocking_workqueue)
{
	/* Force the real CTS input inactive without an external UART adapter. A timer
	 * releases it even if a regressed driver busy-waits inside the work queue. */
	gpio_set_inover(10, GPIO_OVERRIDE_HIGH);
	atomic_clear(&work_ran);
	k_timer_start(&cts_rescue, K_MSEC(100), K_NO_WAIT);
	const int64_t start = k_uptime_get();
	zassert_true(zlrs_serial_receive(bytes, sizeof(bytes)));
	zassert_true(k_work_submit(&marker) >= 0);
	k_msleep(10);
	const int64_t elapsed = k_uptime_get() - start;
	const bool responsive = atomic_get(&work_ran) && elapsed < 50;
	const bool blocked = state().tx_queued > 0U && !(uart1_hw->fr & UART_UARTFR_CTS_BITS);
	/* Recover before assertions, so failures still reach MCUboot rollback. */
	gpio_set_inover(10, GPIO_OVERRIDE_LOW);
	for (int i = 0; i < 100 && state().tx_queued != 0U; i++) {
		k_msleep(1);
	}
	k_msleep(30);
	k_timer_stop(&cts_rescue);
	gpio_set_inover(10, GPIO_OVERRIDE_NORMAL);
	printk("CTS blocked=%d work_responsive=%d wake_ms=%lld tx_queued=%u\n", blocked, responsive,
	       (long long)elapsed, state().tx_queued);
	zassert_true(responsive, "CTS stalled the work queue for %lld ms", (long long)elapsed);
	zassert_true(blocked, "TX did not wait for CTS");
	zassert_equal(state().tx_queued, 0U);
}

static uint8_t pattern_byte(uint32_t offset)
{
	return (uint8_t)((offset * 17U) ^ (offset >> 8) ^ 0x5aU);
}

static void check_bytes(uint32_t offset, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		zassert_equal(bytes[i], pattern_byte(offset + i), "byte %u", offset + i);
	}
}

ZTEST(serial_hw, test_rts_backpressure_and_ring_wrap)
{
	/* UARTCR.LBE loops TX/RX and RTS/CTS through the real peripheral. No wires
	 * or host adapter are needed; physical cable connectivity is a separate check. */
	hw_set_bits(&uart1_hw->cr, UART_UARTCR_LBE_BITS);
	const zlrs_serial_state_t initial = state();
	const uint32_t total = 6147U;
	uint32_t queued = 0U;
	int64_t deadline = k_uptime_get() + 2000;
	while (queued < total && k_uptime_get() < deadline) {
		const uint32_t count = MIN(sizeof(bytes), total - queued);
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = pattern_byte(queued + i);
		}
		if (zlrs_serial_receive(bytes, count)) {
			queued += count;
		} else {
			k_msleep(1);
		}
	}
	zassert_equal(queued, total);
	while ((!state().rx_paused || state().rx_queued != 4096U) && k_uptime_get() < deadline) {
		k_msleep(1);
	}
	zassert_true(state().rx_paused);
	zassert_equal(state().rx_queued, 4096U);
	k_msleep(10);
	const zlrs_serial_state_t stalled = state();
	zassert_true(stalled.tx_queued > 0U);
	zassert_false(uart1_hw->fr & UART_UARTFR_CTS_BITS);
	zassert_true(io_bank0_hw->io[11].status & IO_BANK0_GPIO0_STATUS_OUTTOPAD_BITS);
	k_msleep(50);
	zassert_equal(state().tx_total, stalled.tx_total, "CTS did not stop transmission");
	zassert_equal(state().rx_total, stalled.rx_total);

	uint32_t received = 0U;
	for (int cycle = 0; cycle < 4; cycle++) {
		zassert_equal(zlrs_serial_read(bytes, 63), 63);
		check_bytes(received, 63);
		received += 63;
		k_msleep(5);
		zassert_true(state().rx_paused);
		zassert_equal(state().rx_queued, 4096 - 63);
		zassert_equal(zlrs_serial_read(bytes, 1), 1);
		check_bytes(received, 1);
		received++;
		deadline = k_uptime_get() + 100;
		while ((!state().rx_paused || state().rx_queued != 4096U) &&
		       k_uptime_get() < deadline) {
			k_msleep(1);
		}
		zassert_true(state().rx_paused);
		zassert_equal(state().rx_queued, 4096U);
	}
	deadline = k_uptime_get() + 2000;
	while (received < total && k_uptime_get() < deadline) {
		const uint32_t count = zlrs_serial_read(bytes, sizeof(bytes));
		check_bytes(received, count);
		received += count;
		k_msleep(1);
	}
	zassert_equal(received, total);
	zassert_equal(state().rx_total - initial.rx_total, total);
	zassert_equal(state().tx_total - initial.tx_total, total);
	zassert_equal(state().rx_dropped, initial.rx_dropped);
	zassert_equal(state().rx_queued, 0U);
	zassert_equal(state().tx_queued, 0U);
	zassert_false(state().rx_paused);
	zassert_ok(uart_err_check(uart));
	printk("RTS/CTS loopback bytes=%u pause_cycles=4 dropped=0\n", received);
}

ZTEST(serial_hw, test_short_tx_restarts_below_fifo_watermark)
{
	hw_set_bits(&uart1_hw->cr, UART_UARTCR_LBE_BITS);
	uint32_t received = 0U;
	for (int burst = 0; burst < 128; burst++) {
		const uint16_t count = 1U + burst % 3U;
		for (uint16_t i = 0; i < count; i++) {
			bytes[i] = pattern_byte(received + i);
		}
		zassert_true(zlrs_serial_receive(bytes, count));
		const int64_t deadline = k_uptime_get() + 50;
		uint16_t got = 0U;
		while (got < count && k_uptime_get() < deadline) {
			const uint16_t n = zlrs_serial_read(bytes, count - got);
			check_bytes(received + got, n);
			got += n;
			k_msleep(1);
		}
		zassert_equal(got, count, "short burst %d never completed", burst);
		received += got;
	}
	zassert_ok(uart_err_check(uart));
	printk("Short TX restarts bursts=128 bytes=%u\n", received);
}

ZTEST_SUITE(serial_hw, NULL, setup, cleanup, cleanup, NULL);
