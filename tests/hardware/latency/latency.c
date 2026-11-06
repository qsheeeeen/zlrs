/* SPDX-License-Identifier: Apache-2.0 */
/* MCU-to-MCU probes through the production byte queue, authentication and ARQ. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include "zlrs/link/link.h"
#include "platform/slot_timer.h"
#include "trace.h"

#define BYTES 32U
#define MAGIC 0x4c52545aU
#define PING 1U
#define PONG 2U
#define TRIGGER 3U

static struct {
	bool active, initiator, triggered, queued, waiting;
	uint8_t tx[BYTES], rx[BYTES];
	uint16_t rx_used;
	uint32_t seq, sent_us, last_rx_us, ready_us;
	uint32_t count, last_us, min_us, max_us, bad, threshold_us, delay_us, phy;
	uint64_t total_us;
} bench;
static atomic_t command;
static uint32_t requested_role, requested_threshold, requested_delay;
static int command_result;
K_SEM_DEFINE(command_done, 0, 1);

uint32_t latency_trace_overwritten(void);
uint16_t __real_zlrs_link_tx_space(const zlrs_link_t *link);
uint16_t __real_zlrs_serial_read(uint8_t *buf, uint16_t len);
bool __real_zlrs_serial_receive(const uint8_t *buf, uint16_t len);

static void queue(uint32_t kind, uint32_t seq, uint32_t sent, uint32_t elapsed)
{
	memset(bench.tx, 0xa5, BYTES);
	sys_put_le32(MAGIC, bench.tx);
	sys_put_le32(kind, bench.tx + 4);
	sys_put_le32(seq, bench.tx + 8);
	sys_put_le32(sent, bench.tx + 12);
	sys_put_le32(elapsed, bench.tx + 16);
	bench.ready_us = zlrs_slot_timer_now_us();
	bench.queued = true;
}

static void trigger(uint32_t elapsed)
{
	if (!bench.triggered) {
		bench.triggered = true;
		zlrs_trace_trigger(bench.seq, elapsed);
		if (bench.initiator) {
			queue(TRIGGER, bench.seq, bench.sent_us, elapsed);
		}
	}
}

uint16_t __wrap_zlrs_link_tx_space(const zlrs_link_t *link)
{
	int op = atomic_set(&command, 0);
	if (op != 0) {
		command_result = 0;
		if (op == 1) {
			struct zlrs_trace_status trace = zlrs_trace_status();
			if (link->queue_count != 0U || link->state != ZLRS_STATE_CONNECTED ||
			    trace.state != ZLRS_TRACE_CAPTURING || trace.duration_ms != 0U) {
				command_result = -EBUSY;
			} else {
				memset(&bench, 0, sizeof(bench));
				bench.active = true;
				bench.initiator = requested_role == 0U;
				bench.threshold_us = requested_threshold;
				bench.delay_us = requested_delay;
			}
		} else {
			bench.active = false;
		}
		k_sem_give(&command_done);
	}
	if (bench.active && !bench.triggered) {
		bench.phy = zlrs_mod_phy_id(&link->cfg.mod);
		uint32_t now = zlrs_slot_timer_now_us();
		if ((bench.initiator && bench.waiting && now - bench.sent_us > 200000U) ||
		    (!bench.initiator && bench.last_rx_us != 0U && now - bench.last_rx_us > 200000U)) {
			trigger(200000U);
		} else if (bench.initiator && !bench.waiting && !bench.queued) {
			queue(PING, ++bench.seq, 0U, 0U);
		}
	}
	return __real_zlrs_link_tx_space(link);
}

uint16_t __wrap_zlrs_serial_read(uint8_t *buf, uint16_t len)
{
	if (!bench.active) {
		return __real_zlrs_serial_read(buf, len);
	}
	uint32_t now = zlrs_slot_timer_now_us();
	if (!bench.queued || len < BYTES || (int32_t)(now - bench.ready_us) < 0) {
		return 0U;
	}
	uint32_t kind = sys_get_le32(bench.tx + 4);
	if (kind == PING) {
		bench.sent_us = now;
		bench.waiting = true;
		sys_put_le32(now, bench.tx + 12);
	}
	ZLRS_TRACE_EVENT("probe_send", sys_get_le32(bench.tx + 8), kind);
	memcpy(buf, bench.tx, BYTES);
	bench.queued = false;
	return BYTES;
}

static void received(void)
{
	uint32_t now = zlrs_slot_timer_now_us();
	uint32_t kind = sys_get_le32(bench.rx + 4), seq = sys_get_le32(bench.rx + 8);
	if (sys_get_le32(bench.rx) != MAGIC || kind < PING || kind > TRIGGER) {
		bench.bad++;
		return;
	}
	for (unsigned int i = 20; i < BYTES; i++) {
		if (bench.rx[i] != 0xa5) {
			bench.bad++;
			return;
		}
	}
	ZLRS_TRACE_EVENT("probe_recv", seq, kind);
	bench.last_rx_us = now;
	if (kind == TRIGGER) {
		bench.seq = seq;
		trigger(sys_get_le32(bench.rx + 16));
	} else if (kind == PING && !bench.initiator && !bench.triggered) {
		bench.seq = seq;
		queue(PONG, seq, sys_get_le32(bench.rx + 12), 0U);
		bench.ready_us += bench.delay_us; /* Nonblocking test injection; zero normally. */
	} else if (kind == PONG && bench.initiator && bench.waiting && seq == bench.seq &&
		   sys_get_le32(bench.rx + 12) == bench.sent_us) {
		uint32_t elapsed = now - bench.sent_us;
		bench.waiting = false;
		bench.last_us = elapsed;
		bench.count++;
		bench.total_us += elapsed;
		bench.min_us = bench.count == 1U ? elapsed : MIN(bench.min_us, elapsed);
		bench.max_us = MAX(bench.max_us, elapsed);
		ZLRS_TRACE_EVENT("probe_rtt", seq, elapsed);
		if (elapsed >= bench.threshold_us) {
			trigger(elapsed);
		}
	}
}

bool __wrap_zlrs_serial_receive(const uint8_t *buf, uint16_t len)
{
	if (!bench.active) {
		return __real_zlrs_serial_receive(buf, len);
	}
	while (len-- != 0U) {
		bench.rx[bench.rx_used++] = *buf++;
		if (bench.rx_used == BYTES) {
			received();
			bench.rx_used = 0U;
		}
	}
	return true;
}

/* Test-only MCUmgr group 69. Control is applied on the RF owner, not this worker. */
static int control(struct smp_streamer *ctxt)
{
	uint32_t op = 0U, role = 0U, threshold = 20000U, delay = 0U;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("op", zcbor_uint32_decode, &op),
		ZCBOR_MAP_DECODE_KEY_DECODER("role", zcbor_uint32_decode, &role),
		ZCBOR_MAP_DECODE_KEY_DECODER("threshold_us", zcbor_uint32_decode, &threshold),
		ZCBOR_MAP_DECODE_KEY_DECODER("delay_us", zcbor_uint32_decode, &delay),
	};
	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded) != 0 ||
	    op > 2U || role > 1U || threshold < 1000U || threshold > 100000U || delay > 100000U) {
		return MGMT_ERR_EINVAL;
	}
	int rc = 0;
	if (op != 0U) {
		requested_role = role;
		requested_threshold = threshold;
		requested_delay = delay;
		k_sem_reset(&command_done);
		atomic_set(&command, op == 1U ? 1 : -1);
		if (k_sem_take(&command_done, K_SECONDS(2)) != 0) {
			atomic_clear(&command);
			rc = -ETIMEDOUT;
		} else {
			rc = command_result;
		}
	}
	unsigned int key = irq_lock();
	__typeof__(bench) status = bench;
	uint32_t overwritten = latency_trace_overwritten();
	irq_unlock(key);
	zcbor_state_t *zs = ctxt->writer->zs;
	bool ok = zcbor_tstr_put_lit(zs, "result") && zcbor_int32_put(zs, rc);
#define FIELD(name, value) ok = ok && zcbor_tstr_put_lit(zs, name) && zcbor_uint64_put(zs, value)
	FIELD("count", status.count);
	FIELD("last_us", status.last_us);
	FIELD("min_us", status.min_us);
	FIELD("max_us", status.max_us);
	FIELD("total_us", status.total_us);
	FIELD("bad", status.bad);
	FIELD("probe_seq", status.seq);
	FIELD("triggered", status.triggered);
	FIELD("active", status.active);
	FIELD("overwritten", overwritten);
	FIELD("delay_us", status.delay_us);
	FIELD("threshold_us", status.threshold_us);
	FIELD("phy", status.phy);
#undef FIELD
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}
static const struct mgmt_handler handlers[] = {{.mh_write = control}};
static struct mgmt_group group = {.mg_handlers = handlers, .mg_handlers_count = 1,
	.mg_group_id = 69};
static void register_control(void) { mgmt_register_group(&group); }
MCUMGR_HANDLER_DEFINE(zlrs_latency_test, register_control);
