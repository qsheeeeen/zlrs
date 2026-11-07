/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Reserve the hash peripheral for the current RF operation, including nested
 * protocol digests. The caller releases it before any radio wait. */
bool zlrs_hash_acquire(uint32_t budget_us);
void zlrs_hash_release(void);
