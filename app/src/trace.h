/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

enum zlrs_trace_command {
	ZLRS_TRACE_START,
	ZLRS_TRACE_STOP,
	ZLRS_TRACE_SAVE,
	ZLRS_TRACE_DISCARD,
};

enum zlrs_trace_state {
	ZLRS_TRACE_IDLE,
	ZLRS_TRACE_CAPTURING,
	ZLRS_TRACE_FROZEN,
	ZLRS_TRACE_SAVING,
	ZLRS_TRACE_SAVED,
};

struct zlrs_trace_status {
	enum zlrs_trace_state state;
	uint32_t id;
	uint32_t duration_ms;
	uint64_t started_ns;
	uint64_t stopped_ns;
	uint32_t reason;
	uint32_t dropped;
	uint32_t generation;
	int slot;
	int error;
};

#if defined(CONFIG_ZLRS_APP_DEBUG_TRACE)
#include <zephyr/tracing/tracing.h>
#include <tracing_core.h>

/* Reset on the MCUmgr worker; control/save on RF owner; stop on kernel timer. */
int zlrs_trace_request(enum zlrs_trace_command command, uint32_t duration_ms);
struct zlrs_trace_status zlrs_trace_status(void);
void zlrs_trace_process(bool maintenance);
void zlrs_trace_maintenance(bool entering);
void zlrs_trace_session(bool entering);
/* Management exit waits until an accepted MCUmgr request and its file access finish. */
bool zlrs_trace_export_busy(void);

/* Avoid CTF's bounded-name copy while recording is disabled. */
#define ZLRS_TRACE_EVENT(name, arg0, arg1) \
	do { \
		if (is_tracing_enabled()) { \
			sys_trace_named_event(name, arg0, arg1); \
		} \
	} while (false)
#else
static inline void zlrs_trace_process(bool maintenance) { ARG_UNUSED(maintenance); }
static inline void zlrs_trace_maintenance(bool entering) { ARG_UNUSED(entering); }
static inline void zlrs_trace_session(bool entering) { ARG_UNUSED(entering); }
static inline bool zlrs_trace_export_busy(void) { return false; }
#define ZLRS_TRACE_EVENT(name, arg0, arg1) ((void)0)
#endif
