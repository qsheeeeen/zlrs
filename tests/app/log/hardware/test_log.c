/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

#include "log.h"

ZTEST(storage_log, test_mount_and_inventory)
{
	zlrs_log_info_t info;

	/* The normal export API mounts on demand; no test-only mount hook is needed. */
	zassert_equal(zlrs_log_info(&info), 0);
	zassert_true(info.count <= ZLRS_LOG_FILES_MAX);
	for (uint8_t i = 1U; i < info.count; i++) {
		zassert_true(info.num[i - 1U] <= info.num[i]);
	}
}

ZTEST_SUITE(storage_log, NULL, NULL, NULL, NULL, NULL);
