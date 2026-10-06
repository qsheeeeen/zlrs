/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private SHA operation required by protocol crypto. Firmware supplies the Zephyr
 * implementation; host tests supply software SHA. No runtime backend selection.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define ZLRS_SHA256_LEN 32U

/* SHA256(first || second), fed without joining buffers. NULL requires zero length.
 * The operation must finish successfully or stop execution; failure must never
 * return bytes that the protocol could accept as an authentication tag or key. */
void zlrs_sha256(const uint8_t *first, size_t first_len, const uint8_t *second,
		 size_t second_len, uint8_t out[ZLRS_SHA256_LEN]);
