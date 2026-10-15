/* SPDX-License-Identifier: Apache-2.0 */
#include "zlrs/proto/frame.h"
#include "../byteorder.h"
#include <string.h>

_Static_assert(ZLRS_PKT_SESS_CFM == ZLRS_FRAME_TYPE_MASK, "eight types require three bits");
_Static_assert(ZLRS_PROTO_VERSION <= 15U, "SYNC version requires a new format above 15");
_Static_assert(ZLRS_PROFILE_P1 == 1, "SYNC profile has one bit for P0/P1");

uint8_t zlrs_profile_tag_len(zlrs_profile_t profile)
{
	return profile == ZLRS_PROFILE_P0 ? 0U : ZLRS_TAG_LEN;
}

static void encode_band_plan(uint8_t *p, const zlrs_band_plan_t *plan)
{
	p[0] = plan->domain | (plan->automatic << 3U) | (plan->target_subg << 4U);
	zlrs_put_le32(p + 1U, plan->switch_frame);
}

static bool decode_band_plan(zlrs_band_plan_t *plan, const uint8_t *p)
{
	if ((p[0] & 0xe0U) != 0U || (p[0] & 7U) > 4U) {
		return false;
	}
	*plan = (zlrs_band_plan_t){.domain = p[0] & 7U,
		.automatic = (p[0] >> 3U) & 1U, .target_subg = (p[0] >> 4U) & 1U,
		.switch_frame = zlrs_get_le32(p + 1U)};
	return plan->automatic || plan->switch_frame == 0U;
}

uint16_t zlrs_frame_len(uint16_t payload_len, uint8_t tag_len)
{
	return (uint16_t)(ZLRS_FRAME_HDR_LEN + payload_len + tag_len);
}

static bool payload_len_valid(uint8_t type, uint16_t payload_len)
{
	switch (type) {
	case ZLRS_PKT_DATA_L2F:
	case ZLRS_PKT_DATA_F2L:
		return payload_len >= ZLRS_DATA_PREFIX_LEN && payload_len <= ZLRS_PAYLOAD_MAX_LEN;
	case ZLRS_PKT_SYNC:
		return payload_len == ZLRS_SYNC_PAYLOAD_LEN;
	case ZLRS_PKT_PAIR_REQ:
		return payload_len == ZLRS_PAIR_REQ_PAYLOAD_LEN;
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM:
		return payload_len == ZLRS_PAIR_PAYLOAD_LEN;
	case ZLRS_PKT_SESS_RESP:
		return payload_len == ZLRS_SESS_RESP_PAYLOAD_LEN;
	case ZLRS_PKT_SESS_CFM:
		return payload_len == ZLRS_SESS_CFM_PAYLOAD_LEN;
	default:
		return false;
	}
}

uint16_t zlrs_frame_encode(const zlrs_frame_t *frame, uint8_t *buf, uint16_t cap,
			   uint16_t len, uint8_t tag_len)
{
	if (frame->seq == 0U || len > cap || len > ZLRS_FRAME_MAX_LEN ||
	    len < ZLRS_FRAME_HDR_LEN + tag_len ||
	    (tag_len != 0U && tag_len != ZLRS_TAG_LEN)) {
		return 0U;
	}
	const uint16_t payload_len = len - ZLRS_FRAME_HDR_LEN - tag_len;
	if (!payload_len_valid(frame->type, payload_len)) {
		return 0U;
	}
	buf[0] = frame->type;
	zlrs_put_le32(buf + 1U, frame->seq);
	uint8_t *const p = buf + ZLRS_FRAME_HDR_LEN;
	switch (frame->type) {
	case ZLRS_PKT_DATA_L2F:
	case ZLRS_PKT_DATA_F2L: {
		const zlrs_data_frame_t *const data = &frame->body.data;
		p[0] = (uint8_t)((data->data_seq << ZLRS_DATA_SEQ_SHIFT) | data->ack);
		p[1] = data->rssi_magnitude;
		p[2] = data->lq_percent;
		memcpy(p + ZLRS_DATA_PREFIX_LEN, data->bytes,
		       payload_len - ZLRS_DATA_PREFIX_LEN);
		break;
	}
	case ZLRS_PKT_SYNC:
		buf[0] |= (uint8_t)((frame->body.sync.profile << ZLRS_SYNC_PROFILE_SHIFT) |
				   (frame->body.sync.version << ZLRS_SYNC_VERSION_SHIFT));
		p[0] = frame->body.sync.hop_index;
		zlrs_put_le32(p + 1U, frame->body.sync.nonce_leader);
		zlrs_put_le32(p + 5U, frame->body.sync.frame_index);
		encode_band_plan(p + 9U, &frame->body.sync.band_plan);
		break;
	case ZLRS_PKT_PAIR_REQ:
		memcpy(p, frame->body.pair_req.dev_a, ZLRS_DEV_ID_LEN);
		zlrs_put_le32(p + 4U, frame->body.pair_req.nonce_a);
		break;
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM:
	{
		const zlrs_pair_frame_t *const pair = &frame->body.pair;
		memcpy(p, pair->dev_a, ZLRS_DEV_ID_LEN);
		memcpy(p + 4U, pair->dev_b, ZLRS_DEV_ID_LEN);
		zlrs_put_le32(p + 8U, pair->nonce_a);
		zlrs_put_le32(p + 12U, pair->nonce_b);
		break;
	}
	case ZLRS_PKT_SESS_RESP:
		zlrs_put_le32(p, frame->body.sess_resp.nonce_leader);
		zlrs_put_le32(p + 4U, frame->body.sess_resp.nonce_follower);
		break;
	case ZLRS_PKT_SESS_CFM:
		zlrs_put_le32(p, frame->body.sess_cfm.nonce_follower);
		p[4] = frame->body.sess_cfm.hop_index;
		zlrs_put_le32(p + 5U, frame->body.sess_cfm.frame_index);
		encode_band_plan(p + 9U, &frame->body.sess_cfm.band_plan);
		break;
	}
	if (tag_len != 0U) {
		memcpy(p + payload_len, frame->tag, tag_len);
	}
	return len;
}

bool zlrs_frame_decode(zlrs_frame_t *frame, const uint8_t *buf, uint16_t len, uint8_t tag_len)
{
	if (len < ZLRS_FRAME_HDR_LEN + tag_len || len > ZLRS_FRAME_MAX_LEN ||
	    (tag_len != 0U && tag_len != ZLRS_TAG_LEN)) {
		return false;
	}
	const uint8_t type = buf[0] & ZLRS_FRAME_TYPE_MASK;
	if ((type != ZLRS_PKT_SYNC && (buf[0] & ~ZLRS_FRAME_TYPE_MASK) != 0U) ||
	    zlrs_get_le32(buf + 1U) == 0U) {
		return false;
	}
	const uint16_t payload_len = len - ZLRS_FRAME_HDR_LEN - tag_len;
	if (!payload_len_valid(type, payload_len)) {
		return false;
	}
	const uint8_t *const p = buf + ZLRS_FRAME_HDR_LEN;
	*frame = (zlrs_frame_t){.type = type};
	frame->seq = zlrs_get_le32(buf + 1U);
	memcpy(frame->tag, p + payload_len, tag_len);
	switch (frame->type) {
	case ZLRS_PKT_DATA_L2F:
	case ZLRS_PKT_DATA_F2L: {
		zlrs_data_frame_t *const data = &frame->body.data;
		data->ack = p[0] & ZLRS_DATA_ACK_MASK;
		data->data_seq = p[0] >> ZLRS_DATA_SEQ_SHIFT;
		data->rssi_magnitude = p[1];
		data->lq_percent = p[2];
		memcpy(data->bytes, p + ZLRS_DATA_PREFIX_LEN,
		       payload_len - ZLRS_DATA_PREFIX_LEN);
		break;
	}
	case ZLRS_PKT_SYNC:
		frame->body.sync.version = buf[0] >> ZLRS_SYNC_VERSION_SHIFT;
		frame->body.sync.profile = (buf[0] >> ZLRS_SYNC_PROFILE_SHIFT) & 1U;
		frame->body.sync.hop_index = p[0];
		frame->body.sync.nonce_leader = zlrs_get_le32(p + 1U);
		frame->body.sync.frame_index = zlrs_get_le32(p + 5U);
		if (!decode_band_plan(&frame->body.sync.band_plan, p + 9U)) {
			return false;
		}
		break;
	case ZLRS_PKT_PAIR_REQ:
		memcpy(frame->body.pair_req.dev_a, p, ZLRS_DEV_ID_LEN);
		frame->body.pair_req.nonce_a = zlrs_get_le32(p + 4U);
		break;
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM:
	{
		zlrs_pair_frame_t *const pair = &frame->body.pair;
		memcpy(pair->dev_a, p, ZLRS_DEV_ID_LEN);
		memcpy(pair->dev_b, p + 4U, ZLRS_DEV_ID_LEN);
		pair->nonce_a = zlrs_get_le32(p + 8U);
		pair->nonce_b = zlrs_get_le32(p + 12U);
		break;
	}
	case ZLRS_PKT_SESS_RESP:
		frame->body.sess_resp.nonce_leader = zlrs_get_le32(p);
		frame->body.sess_resp.nonce_follower = zlrs_get_le32(p + 4U);
		break;
	case ZLRS_PKT_SESS_CFM:
		frame->body.sess_cfm.nonce_follower = zlrs_get_le32(p);
		frame->body.sess_cfm.hop_index = p[4];
		frame->body.sess_cfm.frame_index = zlrs_get_le32(p + 5U);
		if (!decode_band_plan(&frame->body.sess_cfm.band_plan, p + 9U)) {
			return false;
		}
		break;
	}
	return true;
}
