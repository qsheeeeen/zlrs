/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

#include "log.h"

ZTEST(storage_log, test_mount_and_inventory)
{
	zlrs_log_info_t info;

	/* The normal export API mounts on demand; no test-only mount hook is needed. */
	zassert_equal(zlrs_log_info(&info), 0);
	zassert_true(info.count <= CONFIG_LOG_BACKEND_FS_FILES_LIMIT);
	for (uint16_t i = 1U; i < info.count; i++) {
		zassert_true(info.num[i - 1U] <= info.num[i]);
	}
}

ZTEST(storage_log, test_backend_filename_contract)
{
	zassert_equal(zlrs_log_file_id(CONFIG_LOG_BACKEND_FS_FILE_PREFIX "0000"), 0);
	zassert_equal(zlrs_log_file_id(CONFIG_LOG_BACKEND_FS_FILE_PREFIX "9999"), 9999);
	const char *invalid[] = {"",
				 "other.0001",
				 CONFIG_LOG_BACKEND_FS_FILE_PREFIX "123",
				 CONFIG_LOG_BACKEND_FS_FILE_PREFIX "00001",
				 CONFIG_LOG_BACKEND_FS_FILE_PREFIX "+001",
				 CONFIG_LOG_BACKEND_FS_FILE_PREFIX " 001",
				 CONFIG_LOG_BACKEND_FS_FILE_PREFIX "0001/extra"};
	for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
		zassert_true(zlrs_log_file_id(invalid[i]) < 0, "%s", invalid[i]);
	}
}

ZTEST_SUITE(storage_log, NULL, NULL, NULL, NULL, NULL);
