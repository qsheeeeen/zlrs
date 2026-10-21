/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "band.h"

/* A fixed-PHY benchmark must not succeed by silently changing its air rate.
 * Keep search and power recovery active; suppress automatic negotiated changes.
 * Explicit profile requests still use the production negotiation path. */
static atomic_t pinned;
static atomic_t suppressed;

zlrs_band_action_t __real_zlrs_band_policy_update(zlrs_band_policy_t *policy,
						  const zlrs_band_observation_t *observation);

zlrs_band_action_t __wrap_zlrs_band_policy_update(zlrs_band_policy_t *policy,
						  const zlrs_band_observation_t *observation)
{
	const zlrs_band_action_t action = __real_zlrs_band_policy_update(policy, observation);
	if (atomic_get(&pinned) && action == ZLRS_BAND_PLAN) {
		atomic_inc(&suppressed);
		return ZLRS_BAND_KEEP;
	}
	return action;
}

static int cmd_phy(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 3U && strcmp(argv[1], "pin") == 0 &&
	    (strcmp(argv[2], "0") == 0 || strcmp(argv[2], "1") == 0)) {
		atomic_set(&pinned, argv[2][0] == '1');
	} else if (argc != 1U) {
		return -EINVAL;
	}
	shell_print(sh, "PHY_PIN active=%u suppressed=%u", (unsigned int)atomic_get(&pinned),
		    (unsigned int)atomic_get(&suppressed));
	return 0;
}

SHELL_CMD_REGISTER(test_phy, NULL, "test-only fixed PHY: pin 0|1, or show state", cmd_phy);
