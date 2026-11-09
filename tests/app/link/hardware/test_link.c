/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/ztest.h>

#include "link/handshake.h"
#include "link/link.h"

ZTEST(link, test_pair_key_rederivation)
{
	static zlrs_link_t link;
	zlrs_link_cfg_t cfg = {
		.role = ZLRS_ROLE_LEADER,
		.region = ZLRS_REGION_ISM2400,

		.profile = ZLRS_PROFILE_P1,
		.data_mtu = 502U,
	};
	const zlrs_persist_state_t saved = {
		.dev_id = {1U, 2U, 3U, 4U},
		.root_key = {1U},
		.pairing = {.peer_id = {5U, 6U, 7U, 8U},
			    .pair_salt = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}},
		.configured = true,
		.paired = true,
	};
	uint8_t first[ZLRS_KEY_LEN];

	cfg.phy = ZLRS_PHY_FLRC_2600;
	zlrs_link_init(&link, &cfg, NULL, NULL, NULL);
	zlrs_link_restore(&link, &saved, 0U);
	memcpy(first, link.pair_key, sizeof(first));
	/* Rebuild and restore exactly as application credential adoption does. */
	zlrs_link_init(&link, &cfg, NULL, NULL, NULL);
	zlrs_link_restore(&link, &saved, 1U);
	zassert_mem_equal(first, link.pair_key, sizeof(first));
}

ZTEST_SUITE(link, NULL, NULL, NULL, NULL, NULL);
