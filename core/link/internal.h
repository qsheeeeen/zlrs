/* Private helpers between DATA link mechanisms. No additional state or callbacks.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "zlrs/link/link.h"
#include <assert.h>
#include <string.h>

#define DATA_FEEDBACK_LEN   2U
#define DATA_ARQ_HEADER_LEN (DATA_FEEDBACK_LEN + 2U)

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

uint16_t link_acq_timeout_ms(const zlrs_link_t *link);
uint16_t link_build_frame(zlrs_link_t *link, uint8_t type, uint8_t flags, const uint8_t *payload,
			  uint16_t len, const uint8_t *tag_key);
void link_derive_by_endpoints(const zlrs_link_t *link, const char *label, const uint8_t *ctx,
			      size_t ctx_len, uint8_t out[ZLRS_KEY_LEN], uint8_t in[ZLRS_KEY_LEN]);
uint16_t link_encode_frame(const zlrs_link_t *link, uint8_t *buf, uint16_t seq, uint8_t type,
			   uint8_t flags, const uint8_t *payload, uint16_t len,
			   const uint8_t *tag_key);
void link_handle_data(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);
bool link_handle_pair_frame(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);
int link_handle_sess_cfm(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);
bool link_handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);
void link_handle_sync(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);
uint32_t link_random(const zlrs_link_t *link, uint32_t salt);
void link_lq_account(zlrs_link_t *link, uint16_t seq);
const zlrs_radio_req_t *link_pair_slot(zlrs_link_t *link, bool down, uint32_t now_ms);
void link_roll_session(zlrs_link_t *link, uint32_t now_ms);
const zlrs_radio_req_t *link_send_data(zlrs_link_t *link, uint32_t now_ms);
const zlrs_radio_req_t *link_send_empty_data(zlrs_link_t *link);
const zlrs_radio_req_t *link_send_sess_cfm(zlrs_link_t *link);
const zlrs_radio_req_t *link_send_sess_resp(zlrs_link_t *link);
const zlrs_radio_req_t *link_send_sync(zlrs_link_t *link, uint32_t now_ms);
void link_set_state(zlrs_link_t *link, zlrs_link_state_t state, uint32_t now_ms);
uint32_t link_slot_freq_hz(const zlrs_link_t *link);
const zlrs_radio_req_t *link_submit_rx(zlrs_link_t *link, uint32_t freq_hz, uint8_t sync_slot,
				       const uint8_t sync_word[ZLRS_SYNC_WORD_LEN],
				       uint16_t timeout_ms);
const zlrs_radio_req_t *link_submit_tx(zlrs_link_t *link, uint16_t len, uint32_t freq_hz,
				       uint8_t sync_slot,
				       const uint8_t sync_word[ZLRS_SYNC_WORD_LEN]);
uint32_t link_sync_freq_hz(const zlrs_link_t *link);
const uint8_t *link_tag_key_for(const zlrs_link_t *link, uint8_t type, bool outbound);
