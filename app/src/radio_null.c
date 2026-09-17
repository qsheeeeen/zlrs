/*
 * 射频移植层（空后端）：CONFIG_ZLRS_APP_RADIO=n 时用这份，接口见 radio_port.h。
 *
 * Core2021 还没接上时整条射频都不参与：不初始化 USP/RAC，也不碰 SPI 与 BUSY
 * 引脚——LR20xx 的 HAL 等不到 BUSY 会 k_oops，模块没接就整机起不来。
 *
 * 每次事务提交后当场按"发出去、这一轮没收到回音"收尾，所以链路层的状态机、
 * 加密、组帧、时隙节奏照常跑，只是永远握不上手；端口（CRSF / 透传 / 配置模式）、
 * 按键、状态灯与 NVS 都不受影响。见 doc/01-system.md 2.3。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "radio_port.h"

LOG_MODULE_REGISTER(zlrs_radio_null, CONFIG_APP_LOG_LEVEL);

typedef struct {
	zlrs_link_t *link; /* 这次事务回填给哪条链路 */
	bool pending;      /* 事务已"完成"，等着 drain 回填 */
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
	/* 没有射频要等，事务提交时就"完成"了 */
	ARG_UNUSED(timeout_us);

	return 0;
}

static void drain_client(null_client_t *client)
{
	if (!client->pending || client->link == NULL) {
		return;
	}

	client->pending = false;

	/* 结果是"发完了、这一轮没收到"：RX 长度 0 就是空接收窗口 */
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
	/* 没有挂在芯片上的事务要撤 */
}

uint32_t zlrs_radio_port_now_ms(void)
{
	return k_uptime_get_32();
}
