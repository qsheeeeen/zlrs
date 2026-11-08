/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <zephyr/sys/atomic.h>

#include "band.h"
#include "fixtures.h"

/* A fixed-PHY benchmark must not succeed by silently changing its air rate.
 * Keep search and power recovery active; suppress automatic negotiated changes.
 * Explicit profile requests still use the production negotiation path. */
static atomic_t pinned;
static atomic_t suppressed;

zlrs_band_action_t __real_zlrs_band_policy_update(zlrs_band_policy_t *policy,
						  const zlrs_link_observation_t *observation,
						  const zlrs_power_t *power);

zlrs_band_action_t __wrap_zlrs_band_policy_update(zlrs_band_policy_t *policy,
						  const zlrs_link_observation_t *observation,
						  const zlrs_power_t *power)
{
	const zlrs_band_action_t action =
		__real_zlrs_band_policy_update(policy, observation, power);
	if (atomic_get(&pinned) && action == ZLRS_BAND_PLAN) {
		atomic_inc(&suppressed);
		return ZLRS_BAND_KEEP;
	}
	return action;
}

int zlrs_test_phy_control(struct zlrs_test_report *report, enum zlrs_test_op op, uint32_t arg)
{
	if (op == TEST_PHY_PIN && arg <= 1U) {
		atomic_set(&pinned, arg);
	} else if (op != TEST_PHY_STATUS) {
		return -EINVAL;
	}
	zlrs_test_print(report, "PHY_PIN active=%u suppressed=%u",
			(unsigned int)atomic_get(&pinned), (unsigned int)atomic_get(&suppressed));
	return 0;
}
