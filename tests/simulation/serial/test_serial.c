/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>
#include <zlrs/usb.h>

#include "serial.h"
#include "usb_state.h"

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(data_uart));
static const struct device *const usb = DEVICE_DT_GET(DT_NODELABEL(data_usb));
static uint8_t pattern[4096];
static uint8_t bytes[4096];

static void message(enum usbd_msg_type type, int status)
{
	const struct usbd_msg msg = {.type = type, .status = status};
	zlrs_usb_message(NULL, &msg);
}

static zlrs_serial_state_t state(const struct device *dev)
{
	zlrs_serial_state_t states[2];
	size_t n = zlrs_serial_state(states, ARRAY_SIZE(states));
	for (size_t i = 0; i < n; i++) {
		if (strcmp(states[i].name, dev->name) == 0) {
			return states[i];
		}
	}
	zassert_unreachable("missing endpoint %s", dev->name);
	return (zlrs_serial_state_t){0};
}

static void selected(bool host)
{
	zlrs_serial_publish_state();
	struct zlrs_serial_status s;
	zassert_ok(zbus_chan_read(&zlrs_serial_chan, &s, K_NO_WAIT));
	const bool use_usb = host && DT_HAS_ALIAS(zlrs_data_cdc);
	const bool use_uart = !use_usb && DT_HAS_ALIAS(zlrs_data_uart);
	zassert_equal(s.usb_active, use_usb);
	zassert_equal(s.ready, use_usb || use_uart);
#if DT_HAS_ALIAS(zlrs_data_cdc)
	zassert_equal(state(usb).active, use_usb);
#endif
#if DT_HAS_ALIAS(zlrs_data_uart)
	zassert_equal(state(uart).active, use_uart);
#endif
}

static void wait_rx(const struct device *dev, uint32_t count)
{
	for (int i = 0; i < 100 && state(dev).rx_queued != count; i++) {
		k_msleep(1);
	}
	zassert_equal(state(dev).rx_queued, count);
}

static void output(const struct device *dev, const uint8_t *expected, size_t count)
{
	size_t got = 0;
	for (int i = 0; i < 200 && got < count; i++) {
		got += uart_emul_get_tx_data(dev, bytes + got, count - got);
		/* The emulator's FIFO consumer does not generate a TX-space interrupt. */
		if (got < count) {
			uart_irq_tx_enable(dev);
		}
		k_msleep(1);
	}
	zassert_equal(got, count, "output length on %s", dev->name);
	zassert_mem_equal(bytes, expected, count);
}

static void *setup(void)
{
	for (size_t i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)(i * 17U + 3U);
	}
	/* Enumeration may happen before the application's serial initialization. */
	message(USBD_MSG_CONFIGURATION, 1);
	zassert_ok(zlrs_serial_init(115200));
	selected(true);
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	message(USBD_MSG_RESET, 0);
	k_msleep(2);
	(void)uart_emul_flush_rx_data(uart);
	(void)uart_emul_flush_rx_data(usb);
	(void)zlrs_serial_read(bytes, sizeof(bytes));
	(void)uart_emul_flush_tx_data(uart);
	(void)uart_emul_flush_tx_data(usb);
	selected(false);
}

ZTEST(serial, test_default_uart_before_input)
{
	/* 5 V presence and control-line updates do not identify a configured host. */
	message(USBD_MSG_VBUS_READY, 0);
	message(USBD_MSG_CDC_ACM_CONTROL_LINE_STATE, 1);
	message(USBD_MSG_RESUME, 0);
	selected(false);
#if DT_HAS_ALIAS(zlrs_data_uart)
	uint32_t tx_total = state(uart).tx_total;
	zassert_true(zlrs_serial_receive(pattern, 33));
	output(uart, pattern, 33);
	zassert_equal(state(uart).tx_total, tx_total + 33);
	/* A receive-only UART sink can bootstrap a local TX-to-RX loopback. */
	wait_rx(uart, 33);
	zassert_equal(zlrs_serial_read(bytes, sizeof(bytes)), 33);
	zassert_mem_equal(bytes, pattern, 33);
#else
	zassert_false(zlrs_serial_receive(pattern, 33));
#endif
}

ZTEST(serial, test_configured_host_without_dtr_or_input)
{
	message(USBD_MSG_CONFIGURATION, 1);
	selected(true);
#if DT_HAS_ALIAS(zlrs_data_cdc)
	uint32_t tx_total = state(usb).tx_total;
	zassert_true(zlrs_serial_receive(pattern, 35));
	output(usb, pattern, 35);
	zassert_equal(state(usb).tx_total, tx_total + 35);
	zassert_equal(uart_emul_get_tx_data(uart, bytes, sizeof(bytes)), 0);
	message(USBD_MSG_CDC_ACM_CONTROL_LINE_STATE, 0);
	selected(true);
#endif
}

ZTEST(serial, test_suspend_resume_and_deconfiguration)
{
	message(USBD_MSG_CONFIGURATION, 1);
	selected(true);
	message(USBD_MSG_SUSPEND, 0);
	selected(false);
	message(USBD_MSG_RESUME, 0);
	selected(true);
	message(USBD_MSG_CONFIGURATION, 0);
	selected(false);
	message(USBD_MSG_RESUME, 0);
	selected(false);
	message(USBD_MSG_SUSPEND, 0);
	message(USBD_MSG_CONFIGURATION, 1);
	selected(true);
}

ZTEST(serial, test_disconnect_reset_and_errors)
{
	const enum usbd_msg_type events[] = {USBD_MSG_VBUS_REMOVED, USBD_MSG_RESET,
					     USBD_MSG_UDC_ERROR, USBD_MSG_STACK_ERROR};

	for (size_t i = 0; i < ARRAY_SIZE(events); i++) {
		message(USBD_MSG_CONFIGURATION, 1);
		selected(true);
		message(events[i], 0);
		selected(false);
		message(USBD_MSG_RESUME, 0);
		selected(false);
	}
}

ZTEST(serial, test_received_bytes_do_not_latch_usb_selection)
{
#if DT_HAS_ALIAS(zlrs_data_cdc)
	message(USBD_MSG_CONFIGURATION, 1);
	zassert_equal(uart_emul_put_rx_data(usb, pattern, 64), 64);
	wait_rx(usb, 64);
	zassert_equal(zlrs_serial_read(bytes, sizeof(bytes)), 64);
	zassert_mem_equal(bytes, pattern, 64);
	message(USBD_MSG_RESET, 0);
	selected(false);
	zassert_equal(uart_emul_put_rx_data(usb, pattern, 64), 64);
	wait_rx(usb, 64);
	uint32_t dropped = zlrs_serial_dropped();
	zassert_equal(zlrs_serial_read(bytes, sizeof(bytes)), 0);
	zassert_equal(zlrs_serial_dropped(), dropped + 64);
	message(USBD_MSG_CONFIGURATION, 1);
	selected(true);
	zassert_equal(zlrs_serial_read(bytes, sizeof(bytes)), 0);
#else
	ztest_test_skip();
#endif
}

ZTEST(serial, test_usb_backpressure_and_resume)
{
#if DT_HAS_ALIAS(zlrs_data_cdc)
	message(USBD_MSG_CONFIGURATION, 1);
	for (size_t i = 0; i < sizeof(pattern); i += 512) {
		zassert_equal(uart_emul_put_rx_data(usb, pattern + i, 512), 512);
		wait_rx(usb, i + 512);
	}
	zassert_equal(uart_emul_put_rx_data(usb, pattern, 128), 128);
	k_msleep(2);
	zassert_true(state(usb).rx_paused);
	zassert_equal(zlrs_serial_read(bytes, 499), 499);
	zassert_mem_equal(bytes, pattern, 499);
	wait_rx(usb, 4096 - 499 + 128);
	zassert_false(state(usb).rx_paused);
	zassert_equal(zlrs_serial_read(bytes, sizeof(bytes)), 4096 - 499 + 128);
	zassert_mem_equal(bytes, pattern + 499, 4096 - 499);
	zassert_mem_equal(bytes + 4096 - 499, pattern, 128);
#else
	ztest_test_skip();
#endif
}

ZTEST(serial, test_packet_acceptance_and_output_ownership)
{
#if DT_HAS_ALIAS(zlrs_data_cdc)
	message(USBD_MSG_CONFIGURATION, 1);
	zassert_true(zlrs_serial_receive(pattern, sizeof(pattern)));
	k_msleep(2);
	zassert_equal(state(usb).tx_queued, 4096 - 64);
	zassert_true(zlrs_serial_receive(pattern, 64));
	zassert_false(zlrs_serial_receive(pattern, 1));
	message(USBD_MSG_RESET, 0);
	selected(false);
#if DT_HAS_ALIAS(zlrs_data_uart)
	zassert_true(zlrs_serial_receive(pattern + 100, 37));
	output(uart, pattern + 100, 37);
#else
	zassert_false(zlrs_serial_receive(pattern + 100, 37));
#endif
	output(usb, pattern, sizeof(pattern));
	output(usb, pattern, 64);
	k_msleep(2);
	zassert_equal(state(usb).tx_queued, 0);
	zassert_equal(uart_emul_get_tx_data(usb, bytes, sizeof(bytes)), 0);
#else
	ztest_test_skip();
#endif
}

ZTEST_SUITE(serial, NULL, setup, before, NULL, NULL);
