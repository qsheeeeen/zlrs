/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "link/link.h"

#ifdef __cplusplus
extern "C" {
#endif

void link_roll_session(zlrs_link_t *link, uint32_t now_ms);

void link_derive_by_endpoints(const zlrs_link_t *link, const char *label, const uint8_t *ctx,
			      size_t ctx_len, uint8_t out[ZLRS_KEY_LEN], uint8_t in[ZLRS_KEY_LEN]);

const zlrs_radio_req_t *link_send_sync(zlrs_link_t *link, uint32_t now_ms);

const zlrs_radio_req_t *link_send_sess_resp(zlrs_link_t *link);

const zlrs_radio_req_t *link_send_sess_cfm(zlrs_link_t *link);

bool link_handle_sync(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);

int link_handle_sess_cfm(zlrs_link_t *link, const zlrs_frame_t *frame, uint16_t len,
			 uint32_t now_ms);

bool link_handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
