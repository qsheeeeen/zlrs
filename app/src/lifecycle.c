/* SPDX-License-Identifier: Apache-2.0 */

#include "lifecycle.h"

#include <string.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(zlrs_lifecycle);

static bool confirmed;

void zlrs_lifecycle_check_rollback(bool fault, uint32_t timeout_ms)
{
	if (confirmed) {
		return;
	}
	if (boot_is_img_confirmed()) {
		confirmed = true;
		return;
	}
	if (fault) {
		LOG_ERR("fault on unconfirmed image: rebooting to roll back");
		k_msleep(2000);
		sys_reboot(SYS_REBOOT_COLD);
	} else if (k_uptime_get_32() >= timeout_ms) {
		LOG_ERR("upgrade not confirmed by host: rebooting to roll back");
		sys_reboot(SYS_REBOOT_COLD);
	}
}

#define RETRY_MIN_MS 1000U
#define RETRY_MAX_MS 30000U

static bool same_pairing(const zlrs_pairing_t *a, const zlrs_pairing_t *b)
{
	return b != NULL && memcmp(a, b, sizeof(*a)) == 0;
}

bool zlrs_pairing_save_due(zlrs_pairing_save_t *state, const zlrs_pairing_t *live,
			   const zlrs_pairing_t *saved, uint32_t now_ms)
{
	if (live == NULL || same_pairing(live, saved)) {
		*state = (zlrs_pairing_save_t){0};
		return false;
	}
	if (state->delay_ms == 0U || !same_pairing(live, &state->candidate)) {
		*state = (zlrs_pairing_save_t){
			.candidate = *live,
			.retry_at_ms = now_ms,
			.delay_ms = RETRY_MIN_MS,
		};
	}
	return (int32_t)(now_ms - state->retry_at_ms) >= 0;
}

uint32_t zlrs_pairing_save_complete(zlrs_pairing_save_t *state, bool success, uint32_t now_ms)
{
	if (success || state->delay_ms == 0U) {
		*state = (zlrs_pairing_save_t){0};
		return 0U;
	}
	const uint32_t delay = state->delay_ms;
	state->retry_at_ms = now_ms + delay;
	state->delay_ms = delay * 2U < RETRY_MAX_MS ? delay * 2U : RETRY_MAX_MS;
	return delay;
}
