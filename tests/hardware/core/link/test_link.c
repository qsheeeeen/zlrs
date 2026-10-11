/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>
#include <string.h>

#include "zlrs/link/link.h"

ZTEST(link, test_pair_key_rederivation)
{
	static zlrs_link_t link;
	zlrs_link_cfg_t cfg = {
		.role = ZLRS_ROLE_TX,
		.domain = ZLRS_DOMAIN_ISM2400,

		.profile = ZLRS_PROFILE_P1,
		.data_mtu = 502U,
	};
	const uint8_t dev_id[ZLRS_DEV_ID_LEN] = {1U, 2U, 3U, 4U};
	const uint8_t peer_id[ZLRS_DEV_ID_LEN] = {5U, 6U, 7U, 8U};
	const uint8_t salt[ZLRS_PAIR_SALT_LEN] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
	const uint8_t root[ZLRS_KEY_LEN] = {1U};
	uint8_t first[ZLRS_KEY_LEN];

	cfg.mod = zlrs_mod_flrc(2600000U);
	zlrs_link_init(&link, &cfg, NULL, NULL, NULL);
	zlrs_link_set_dev_id(&link, dev_id);
	zlrs_link_set_root_key(&link, root, 0U);
	zlrs_link_set_peer(&link, peer_id, salt, 0U);
	memcpy(first, link.pair_key, sizeof(first));
	/* Exercise the production root-key update path rather than a rekey test hook. */
	zlrs_link_set_root_key(&link, root, 1U);
	zassert_mem_equal(first, link.pair_key, sizeof(first));
}

ZTEST_SUITE(link, NULL, NULL, NULL, NULL, NULL);
