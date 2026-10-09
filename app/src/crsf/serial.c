/* SPDX-License-Identifier: Apache-2.0 */
#include "crsf/serial.h"

#include <hardware/gpio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "platform/slot_timer.h"

LOG_MODULE_REGISTER(zlrs_link_crsf, CONFIG_APP_LOG_LEVEL);

#define CRSF_QUEUE_LEN 16U
#define CRSF_PORT      DT_ALIAS(zlrs_crsf_port)
#define CRSF_TX_PIN    DT_GPIO_PIN(CRSF_PORT, tx_gpios)
#define CRSF_RC_AGE_MS CONFIG_ZLRS_APP_CRSF_TIMEOUT_MS

enum {
	CRSF_UART,
	CRSF_USB
};
struct crsf_output {
	uint8_t frame[64];
	uint8_t len;
	uint32_t queued_ms;
	bool latest;
};
struct crsf_ep {
	const struct device *dev;
	zlrs_crsf_parser_t parser;
	struct crsf_output tx[CRSF_QUEUE_LEN];
	uint8_t head;
	uint8_t count;
	uint8_t offset;
	uint32_t last_rx_ms;
	uint32_t last_frame_us;
	uint32_t interval_us;
	uint32_t baud;
	uint32_t pending_baud;
	uint32_t speed_deadline_ms;
	bool seen;
	bool ready;
	bool transmitting;
	bool replied;
};
static struct crsf_ep endpoints[2] = {
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_control_uart))
	[CRSF_UART].dev = DEVICE_DT_GET(DT_ALIAS(zlrs_control_uart)),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_control_cdc))
	[CRSF_USB].dev = DEVICE_DT_GET(DT_ALIAS(zlrs_control_cdc)),
#endif
};
static zlrs_link_crsf_input_t inputs[CRSF_QUEUE_LEN];
static uint8_t input_head;
static uint8_t input_count;
static zlrs_link_crsf_input_t rc_input;
static bool rc_pending;
static struct crsf_ep *source;
static uint32_t dropped;
static bool handset;
static struct k_work tx_work;
static const struct device *const release_timer =
	DEVICE_DT_GET(DT_PHANDLE(CRSF_PORT, release_timer));
static void tx_finish(const struct device *dev, uint8_t channel, uint32_t ticks, void *ctx);
static bool release_pending;

static void release_after(uint32_t us)
{
	struct counter_alarm_cfg alarm = {.ticks = us, .callback = tx_finish};

	release_pending = true;
	if (counter_set_channel_alarm(release_timer, 0U, &alarm) != 0) {
		/* A higher-priority RF interrupt can overtake a short alarm while it is armed. */
		alarm.ticks = 100U;
		(void)counter_set_channel_alarm(release_timer, 0U, &alarm);
	}
}

static struct crsf_ep *active(void)
{
	struct crsf_ep *uart = &endpoints[CRSF_UART];
	struct crsf_ep *usb = &endpoints[CRSF_USB];
	uint32_t dtr = 0U;

	if (!usb->ready) {
		return uart->ready ? uart : NULL;
	}
	(void)uart_line_ctrl_get(usb->dev, UART_LINE_CTRL_DTR, &dtr);
	/* DTR is absent with some USB/IP hosts, so recent valid traffic also selects USB.
	 * A receive-only FC always gets UART unless USB is actually in use. */
	if (dtr != 0U ||
	    (usb->seen && (uint32_t)(zlrs_slot_timer_now_ms() - usb->last_rx_ms) < 1000U)) {
		return usb;
	}
	return uart->ready ? uart : usb;
}

static void high_impedance(void)
{
	gpio_set_function(CRSF_TX_PIN, GPIO_FUNC_SIO);
	gpio_set_dir(CRSF_TX_PIN, GPIO_IN);
	gpio_disable_pulls(CRSF_TX_PIN);
}

static void select_source(struct crsf_ep *ep)
{
	if (source == ep) {
		return;
	}
	input_head = input_count = 0U;
	rc_pending = false;
	for (size_t i = 0U; i < ARRAY_SIZE(endpoints); i++) {
		struct crsf_ep *old = &endpoints[i];

		old->count = old->transmitting && old->offset != 0U ? 1U : 0U;
	}
	source = ep;
}

static void kick(struct crsf_ep *ep)
{
	if (ep == NULL || !ep->ready || ep != active() || ep->count == 0U || ep->transmitting) {
		return;
	}
	const uint32_t now_us = zlrs_slot_timer_now_us();
	struct crsf_output *out = &ep->tx[ep->head];

	if (out->frame[2] == ZLRS_CRSF_TYPE_RC_CHANNELS &&
	    (uint32_t)(zlrs_slot_timer_now_ms() - out->queued_ms) > CRSF_RC_AGE_MS) {
		ep->head = (uint8_t)((ep->head + 1U) % CRSF_QUEUE_LEN);
		ep->count--;
		return;
	}
	if (handset && ep == &endpoints[CRSF_UART]) {
		const uint32_t wire_us =
			((uint32_t)out->len * 10000000U + ep->baud - 1U) / ep->baud;
		const uint32_t interval = ep->interval_us != 0U ? ep->interval_us : 4000U;
		const int32_t age = (int32_t)(now_us - ep->last_frame_us);

		/* One response per master frame; leave time for line release before its next frame. */
		if (!ep->seen || ep->replied || age < 0 ||
		    (uint32_t)age + wire_us + 50U >= interval) {
			return;
		}
		ep->replied = true;
		gpio_set_function(CRSF_TX_PIN, GPIO_FUNC_UART);
	}
	ep->transmitting = true;
	uart_irq_tx_enable(ep->dev);
}

static void tx_start(struct k_work *work)
{
	ARG_UNUSED(work);
	const unsigned int key = irq_lock();

	kick(active());
	irq_unlock(key);
}

static void tx_finish(const struct device *dev, uint8_t channel, uint32_t ticks, void *ctx)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(ticks);
	ARG_UNUSED(ctx);
	struct crsf_ep *ep = &endpoints[CRSF_UART];

	if (!ep->transmitting) {
		release_pending = false;
		return;
	}
	if (!uart_irq_tx_complete(ep->dev)) {
		release_after(20U);
		return;
	}
	if (handset) {
		/* The separate RX pin can see our own response when the two pads are tied. */
		uint8_t echo[32];

		while (uart_fifo_read(ep->dev, echo, sizeof(echo)) > 0) {
		}
		high_impedance();
	}
	ep->transmitting = false;
	release_pending = false;
	if (!handset) {
		k_work_submit(&tx_work);
	}
}

static void isr(const struct device *dev, void *user_data)
{
	struct crsf_ep *ep = user_data;
	const unsigned int key = irq_lock();

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t bytes[32];
			const int n = uart_fifo_read(dev, bytes, sizeof(bytes));
			const uint32_t now_ms = zlrs_slot_timer_now_ms();
			const uint32_t now_us = zlrs_slot_timer_now_us();

			if (handset && ep == &endpoints[CRSF_UART] && ep->transmitting) {
				continue; /* local echo */
			}
			for (int i = 0; i < n; i++) {
				if (zlrs_crsf_feed_at(&ep->parser, bytes[i], now_ms) !=
				    ZLRS_CRSF_FRAME) {
					continue;
				}
				const uint32_t interval = now_us - ep->last_frame_us;

				if (ep->seen && interval >= 1000U && interval <= 100000U) {
					ep->interval_us = interval;
				}
				ep->seen = true;
				ep->last_rx_ms = now_ms;
				ep->last_frame_us = now_us;
				ep->replied = false;
				if (ep != active()) {
					continue;
				}
				select_source(ep);
				const bool rc = ep->parser.type == ZLRS_CRSF_TYPE_RC_CHANNELS;

				if (!rc && input_count == CRSF_QUEUE_LEN) {
					dropped++;
					continue;
				}
				zlrs_link_crsf_input_t *input =
					rc ? &rc_input
					   : &inputs[(input_head + input_count) % CRSF_QUEUE_LEN];

				input->len = (uint8_t)(ep->parser.payload_len + 4U);
				memcpy(input->frame, ep->parser.buf, input->len);
				input->received_ms = now_ms;
				input->received_us = now_us;
				if (rc) {
					rc_pending = true;
				} else {
					input_count++;
				}
				k_work_submit(&tx_work);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			if (ep->count == 0U || !ep->transmitting) {
				uart_irq_tx_disable(dev);
				continue;
			}
			struct crsf_output *out = &ep->tx[ep->head];
			const int sent =
				uart_fifo_fill(dev, out->frame + ep->offset, out->len - ep->offset);

			if (sent > 0) {
				ep->offset += (uint8_t)sent;
			}
			if (ep->offset == out->len) {
				ep->offset = 0U;
				ep->head = (uint8_t)((ep->head + 1U) % CRSF_QUEUE_LEN);
				ep->count--;
				uart_irq_tx_disable(dev);
				if (ep == &endpoints[CRSF_UART]) {
					release_after(((uint32_t)MIN(out->len, 32U) * 10000000U +
						       ep->baud - 1U) /
							      ep->baud +
						      20U);
				} else {
					ep->transmitting = false;
					k_work_submit(&tx_work);
				}
			}
		}
	}
	irq_unlock(key);
}

int zlrs_link_crsf_init(bool is_handset)
{
	handset = is_handset;
	k_work_init(&tx_work, tx_start);
	if (!device_is_ready(release_timer) || counter_start(release_timer) != 0) {
		return -1;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(endpoints); i++) {
		struct crsf_ep *ep = &endpoints[i];

		if (ep->dev == NULL || !device_is_ready(ep->dev)) {
			continue;
		}
		zlrs_crsf_parser_init(&ep->parser);
		if (i == CRSF_UART) {
			struct uart_config cfg;

			ep->baud = handset ? DT_PROP(CRSF_PORT, handset_baud)
					   : DT_PROP(CRSF_PORT, fc_baud);
			if (uart_config_get(ep->dev, &cfg) != 0) {
				return -1;
			}
			cfg.baudrate = ep->baud;
			if (uart_configure(ep->dev, &cfg) != 0) {
				return -1;
			}
			if (handset) {
				high_impedance();
			}
		}
		if (uart_irq_callback_user_data_set(ep->dev, isr, ep) != 0) {
			continue;
		}
		ep->ready = true;
		uart_irq_rx_enable(ep->dev);
	}
	return active() != NULL ? 0 : -1;
}

bool zlrs_link_crsf_ready(void)
{
	return active() != NULL;
}

static bool write_frame(const uint8_t *frame, uint8_t len, bool latest)
{
	if (!zlrs_crsf_frame_valid(frame, len)) {
		return false;
	}
	const unsigned int key = irq_lock();
	struct crsf_ep *ep = active();

	if (ep == NULL || (latest && ep->pending_baud != 0U)) {
		irq_unlock(key);
		return false;
	}
	select_source(ep);
	struct crsf_output *out = NULL;

	if (latest) {
		for (uint8_t i = 0U; i < ep->count; i++) {
			struct crsf_output *queued = &ep->tx[(ep->head + i) % CRSF_QUEUE_LEN];

			if (queued->latest && queued->frame[2] == frame[2] &&
			    (i != 0U || ep->offset == 0U)) {
				out = queued;
				break;
			}
		}
	}
	if (out == NULL) {
		if (ep->count == CRSF_QUEUE_LEN) {
			irq_unlock(key);
			return false;
		}
		out = &ep->tx[(ep->head + ep->count) % CRSF_QUEUE_LEN];
		ep->count++;
	}

	memcpy(out->frame, frame, len);
	out->frame[0] = handset ? ZLRS_CRSF_ADDR_HANDSET : ZLRS_CRSF_ADDR_FC;
	out->len = len;
	out->queued_ms = zlrs_slot_timer_now_ms();
	out->latest = latest;
	irq_unlock(key);
	k_work_submit(&tx_work);
	return true;
}

bool zlrs_link_crsf_write_frame(const uint8_t *frame, uint8_t len)
{
	return write_frame(frame, len, false);
}

uint8_t zlrs_link_crsf_write(uint8_t type, const uint8_t *payload, uint8_t len)
{
	uint8_t frame[64];
	const uint8_t size = zlrs_crsf_encode(
		frame, handset ? ZLRS_CRSF_ADDR_HANDSET : ZLRS_CRSF_ADDR_FC, type, payload, len);

	return zlrs_link_crsf_write_frame(frame, size) ? size : 0U;
}

uint8_t zlrs_link_crsf_write_latest(uint8_t type, const uint8_t *payload, uint8_t len)
{
	uint8_t frame[64];
	const uint8_t size = zlrs_crsf_encode(
		frame, handset ? ZLRS_CRSF_ADDR_HANDSET : ZLRS_CRSF_ADDR_FC, type, payload, len);

	return write_frame(frame, size, true) ? size : 0U;
}

bool zlrs_link_crsf_read(zlrs_link_crsf_input_t *input)
{
	const unsigned int key = irq_lock();

	select_source(active());
	if (input_count == 0U) {
		irq_unlock(key);
		return false;
	}
	*input = inputs[input_head];
	input_head = (uint8_t)((input_head + 1U) % CRSF_QUEUE_LEN);
	input_count--;
	irq_unlock(key);
	return true;
}

bool zlrs_link_crsf_read_rc(zlrs_link_crsf_input_t *input)
{
	const unsigned int key = irq_lock();

	select_source(active());
	const bool ready = rc_pending;

	if (ready) {
		*input = rc_input;
		rc_pending = false;
	}
	irq_unlock(key);
	return ready;
}

void zlrs_link_crsf_flush(void)
{
	const unsigned int key = irq_lock();

	input_head = input_count = 0U;
	rc_pending = false;
	for (size_t i = 0U; i < ARRAY_SIZE(endpoints); i++) {
		struct crsf_ep *ep = &endpoints[i];

		zlrs_crsf_parser_init(&ep->parser);
		/* An already transmitting frame must finish intact. Discard everything after it. */
		ep->count = ep->transmitting && ep->offset != 0U ? 1U : 0U;
	}
	irq_unlock(key);
}

bool zlrs_link_crsf_speed(uint8_t port, uint32_t baud)
{
	struct crsf_ep *ep = active();

	if (port != 0U || ep != &endpoints[CRSF_UART] ||
	    (baud != 115200U && baud != 400000U && baud != 416666U && baud != 420000U &&
	     baud != 1000000U && baud != 2000000U)) {
		return false;
	}
	ep->pending_baud = baud;
	ep->speed_deadline_ms = zlrs_slot_timer_now_ms() + 1000U;
	return true;
}

void zlrs_link_crsf_poll(void)
{
	struct crsf_ep *ep = &endpoints[CRSF_UART];

	if (ep->ready && ep->pending_baud != 0U && ep->count == 0U && !ep->transmitting) {
		struct uart_config cfg;

		if (uart_config_get(ep->dev, &cfg) == 0) {
			cfg.baudrate = ep->pending_baud;
			if (uart_configure(ep->dev, &cfg) == 0) {
				ep->baud = ep->pending_baud;
				ep->speed_deadline_ms = zlrs_slot_timer_now_ms() + 1000U;
				ep->seen = false;
			}
		}
		ep->pending_baud = 0U;
		zlrs_crsf_parser_init(&ep->parser);
	}
	/* Without valid traffic after negotiation, restore the role's default UART speed. */
	if (ep->ready && ep->speed_deadline_ms != 0U &&
	    (int32_t)(zlrs_slot_timer_now_ms() - ep->speed_deadline_ms) >= 0) {
		if (!ep->seen || (int32_t)(ep->last_rx_ms - (ep->speed_deadline_ms - 1000U)) <= 0) {
			struct uart_config cfg;

			if (ep->transmitting) {
				return;
			}
			if (!ep->transmitting && uart_config_get(ep->dev, &cfg) == 0) {
				cfg.baudrate = handset ? DT_PROP(CRSF_PORT, handset_baud)
						       : DT_PROP(CRSF_PORT, fc_baud);
				(void)uart_configure(ep->dev, &cfg);
				ep->baud = cfg.baudrate;
				zlrs_crsf_parser_init(&ep->parser);
			}
		}
		ep->pending_baud = 0U;
		ep->speed_deadline_ms = 0U;
	}
	/* Recover a late alarm without relying on it to release the single-wire line. */
	const unsigned int key = irq_lock();

	if (release_pending && ep->transmitting && ep->offset == 0U &&
	    uart_irq_tx_complete(ep->dev)) {
		(void)counter_cancel_channel_alarm(release_timer, 0U);
		tx_finish(release_timer, 0U, 0U, NULL);
	}
	irq_unlock(key);
	k_work_submit(&tx_work);
}

uint32_t zlrs_link_crsf_rx_dropped(void)
{
	return dropped;
}
