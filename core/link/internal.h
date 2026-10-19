/* Private helpers between DATA link mechanisms. No additional state or callbacks.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "zlrs/link/link.h"
#include "../byteorder.h"
#include <assert.h>
#include <string.h>

uint16_t link_acq_timeout_ms(const zlrs_link_t *link);
bool link_band_plan_valid(const zlrs_link_t *link, const zlrs_band_plan_t *plan,
			  uint32_t frame_index);
void link_adopt_band_plan(zlrs_link_t *link, const zlrs_band_plan_t *plan);
uint16_t link_build_frame(zlrs_link_t *link, zlrs_frame_t *frame, uint16_t payload_len,
			  const uint8_t *tag_key);
void link_derive_by_endpoints(const zlrs_link_t *link, const char *label, const uint8_t *ctx,
			      size_t ctx_len, uint8_t out[ZLRS_KEY_LEN], uint8_t in[ZLRS_KEY_LEN]);
uint16_t link_encode_frame(const zlrs_link_t *link, uint8_t *buf, const zlrs_frame_t *frame,
			   uint16_t payload_len, const uint8_t *tag_key);
void link_handle_data(zlrs_link_t *link, const zlrs_frame_t *frame, uint16_t data_len,
		      uint32_t now_ms);
bool link_handle_pair_frame(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);
int link_handle_sess_cfm(zlrs_link_t *link, const zlrs_frame_t *frame, uint16_t len,
			 uint32_t now_ms);
bool link_handle_sess_resp(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);
bool link_handle_sync(zlrs_link_t *link, const zlrs_frame_t *frame, uint32_t now_ms);
bool link_new_nonce(const zlrs_link_t *link, uint32_t previous, bool have_previous, uint32_t *out);
void link_lq_account(zlrs_link_t *link, uint32_t seq);
const zlrs_radio_req_t *link_pair_slot(zlrs_link_t *link, bool leader_slot, uint32_t now_ms);
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
