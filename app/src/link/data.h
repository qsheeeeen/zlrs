/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "link/link.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Room left in the air-side queue. Callers that own a byte source (a UART, a CDC) should take no
 * more than this, leaving excess bytes in their endpoint until queue space is available.
 */
uint16_t zlrs_link_tx_space(const zlrs_link_t *link);

/* RF owner may write directly into the tail payload. No other link operation
 * is allowed between reserve and commit; commit(0) cancels the reservation. */
uint8_t *zlrs_link_tx_reserve(zlrs_link_t *link, uint16_t *capacity);

void zlrs_link_tx_commit(zlrs_link_t *link, uint16_t written);

/* Complete the immutable DATA attempt and retire best-effort bytes on success. */
void link_complete_data(zlrs_link_t *link, bool success, uint32_t now_ms);

bool link_arq_retry_due(const zlrs_link_t *link, uint32_t now_ms);

/* Build the selected payload in place; the link owner submits the planned radio request. */
uint8_t *link_build_data(zlrs_link_t *link, uint16_t data_len, uint16_t *size);

void link_handle_data(zlrs_link_t *link, const zlrs_data_view_t *view, uint32_t now_ms);

void link_quality_reset(zlrs_link_t *link);

void link_lq_account(zlrs_link_t *link, uint32_t seq);

#ifdef __cplusplus
}
#endif
