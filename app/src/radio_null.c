/*
 * Null radio backend (CONFIG_ZLRS_APP_RADIO=n, interface in radio_port.h):
 * it touches no SPI or BUSY (the LR20xx HAL k_oops()es without BUSY, so an unplugged module
 * would never boot). Transactions finish as "sent, no answer": the state machine still runs.
 * Use cases: doc/12-debug.md 3.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "radio_port.h"

LOG_MODULE_REGISTER(zlrs_radio_null, CONFIG_APP_LOG_LEVEL);

typedef struct {
	zlrs_link_t *link;
	bool pending; /* transaction already "finished", waiting for drain to hand it over */
} null_client_t;

static null_client_t control_client;
static null_client_t data_client;

static uint32_t null_random(void *ctx)
{
	ARG_UNUSED(ctx);

	return sys_rand32_get();
}

static int null_submit(void *ctx, const zlrs_radio_req_t *req)
{
	null_client_t *client = ctx;

	ARG_UNUSED(req);

	client->pending = true;

	return 0;
}

static const zlrs_radio_ops_t control_ops = {
	.submit = null_submit,
	.random = null_random,
	.ctx = &control_client,
};

static const zlrs_radio_ops_t data_ops = {
	.submit = null_submit,
	.random = null_random,
	.ctx = &data_client,
};

const zlrs_radio_ops_t *zlrs_radio_control_ops(void)
{
	return &control_ops;
}

const zlrs_radio_ops_t *zlrs_radio_data_ops(void)
{
	return &data_ops;
}

int zlrs_radio_port_init(zlrs_link_t *control, zlrs_link_t *data)
{
	control_client.link = control;
	data_client.link = data;

	LOG_WRN("radio off: transactions complete locally, nothing goes on air");

	return 0;
}

int zlrs_radio_port_wait_us(uint32_t timeout_us)
{
	/* Nothing to wait for without a radio: the transaction is finished the moment it is submitted */
	ARG_UNUSED(timeout_us);

	return 0;
}

static void drain_client(null_client_t *client)
{
	if (!client->pending || client->link == NULL) {
		return;
	}

	client->pending = false;

	/* The result is "sent, nothing heard": an RX length of 0 is an empty receive window */
	zlrs_link_complete(client->link, ZLRS_RADIO_OK, 0U, 0, 0,
			   zlrs_radio_port_now_ms());
}

void zlrs_radio_port_drain(void)
{
	drain_client(&control_client);
	drain_client(&data_client);
}

void zlrs_radio_port_abort(void)
{
	/* Nothing is pending on the chip, so nothing to abort */
}

uint32_t zlrs_radio_port_now_ms(void)
{
	return k_uptime_get_32();
}
