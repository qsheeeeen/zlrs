/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

#include "storage/log.h"

ZTEST(storage_log, test_mount_and_inventory)
{
	zlrs_mgmt_log_info_t info;

	/* The normal export API mounts on demand; no test-only mount hook is needed. */
	zassert_equal(zlrs_storage_log_info(&info), ZLRS_MGMT_OK);
	zassert_true(info.count <= ZLRS_MGMT_LOG_FILES_MAX);
	if (info.count > 0U) {
		zassert_true(info.newest < info.count);
	}
}

ZTEST_SUITE(storage_log, NULL, NULL, NULL, NULL, NULL);
