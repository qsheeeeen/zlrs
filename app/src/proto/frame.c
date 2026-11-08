/* SPDX-License-Identifier: Apache-2.0 */
#include "proto/frame.h"
#include "proto/mod.h"
#include <zephyr/sys/byteorder.h>
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
	p[0] = plan->region | (plan->automatic << 3U) | (plan->target_subg << 4U);
	sys_put_le32(plan->switch_frame, p + 1U);
	p[5] = plan->phy | (plan->target_phy << 3U);
}

static bool decode_band_plan(zlrs_band_plan_t *plan, const uint8_t *p)
{
	if ((p[0] & 0xe0U) != 0U || (p[0] & 7U) >= ZLRS_REGION_COUNT || (p[5] & 0xc0U) != 0U ||
	    (p[5] & 7U) >= ZLRS_PHY_COUNT || ((p[5] >> 3U) & 7U) >= ZLRS_PHY_COUNT) {
		return false;
	}
	*plan = (zlrs_band_plan_t){.region = p[0] & 7U,
				   .automatic = (p[0] >> 3U) & 1U,
				   .target_subg = (p[0] >> 4U) & 1U,
				   .phy = p[5] & 7U,
				   .target_phy = (p[5] >> 3U) & 7U,
				   .switch_frame = sys_get_le32(p + 1U)};
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

uint16_t zlrs_frame_encode_data(uint8_t *buf, uint16_t cap, zlrs_pkt_type_t type, uint32_t seq,
				const zlrs_data_header_t *header, uint16_t data_len,
				uint8_t tag_len)
{
	const uint32_t total = ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN + data_len + tag_len;
	if (buf == NULL || header == NULL || seq == 0U ||
	    (type != ZLRS_PKT_DATA_L2F && type != ZLRS_PKT_DATA_F2L) ||
	    (tag_len != 0U && tag_len != ZLRS_TAG_LEN) || total > cap ||
	    total > ZLRS_FRAME_MAX_LEN || header->data_seq > ZLRS_DATA_SEQ_MASK ||
	    header->lq_percent > ZLRS_LQ_VALUE_MASK) {
		return 0U;
	}
	buf[0] = type;
	sys_put_le32(seq, buf + 1U);
	uint8_t *p = buf + ZLRS_FRAME_HDR_LEN;
	p[0] = (header->data_seq << ZLRS_DATA_SEQ_SHIFT) | header->ack;
	p[1] = header->rssi_magnitude;
	p[2] = header->lq_percent | (header->lq_toggle ? ZLRS_LQ_TOGGLE_MASK : 0U);
	return (uint16_t)total;
}

bool zlrs_frame_decode_data(zlrs_data_view_t *view, const uint8_t *buf, uint16_t len,
			    uint8_t tag_len)
{
	if (buf == NULL || view == NULL || (tag_len != 0U && tag_len != ZLRS_TAG_LEN) ||
	    len < ZLRS_FRAME_HDR_LEN + ZLRS_DATA_PREFIX_LEN + tag_len || len > ZLRS_FRAME_MAX_LEN ||
	    (buf[0] != ZLRS_PKT_DATA_L2F && buf[0] != ZLRS_PKT_DATA_F2L) ||
	    sys_get_le32(buf + 1U) == 0U) {
		return false;
	}
	const uint8_t *p = buf + ZLRS_FRAME_HDR_LEN;
	*view = (zlrs_data_view_t){
		.seq = sys_get_le32(buf + 1U),
		.type = buf[0],
		.header = {.data_seq = p[0] >> ZLRS_DATA_SEQ_SHIFT,
			   .ack = (p[0] & ZLRS_DATA_ACK_MASK) != 0U,
			   .rssi_magnitude = p[1],
			   .lq_percent = p[2] & ZLRS_LQ_VALUE_MASK,
			   .lq_toggle = (p[2] & ZLRS_LQ_TOGGLE_MASK) != 0U},
		.bytes = p + ZLRS_DATA_PREFIX_LEN,
		.tag = buf + len - tag_len,
		.len = len - ZLRS_FRAME_HDR_LEN - ZLRS_DATA_PREFIX_LEN - tag_len,
	};
	return true;
}

uint16_t zlrs_frame_encode(const zlrs_frame_t *frame, uint8_t *buf, uint16_t cap, uint16_t len,
			   uint8_t tag_len)
{
	if (frame->type <= ZLRS_PKT_DATA_F2L || frame->seq == 0U || len > cap || len > ZLRS_FRAME_MAX_LEN ||
	    len < ZLRS_FRAME_HDR_LEN + tag_len || (tag_len != 0U && tag_len != ZLRS_TAG_LEN)) {
		return 0U;
	}
	const uint16_t payload_len = len - ZLRS_FRAME_HDR_LEN - tag_len;
	if (!payload_len_valid(frame->type, payload_len)) {
		return 0U;
	}
	buf[0] = frame->type;
	sys_put_le32(frame->seq, buf + 1U);
	uint8_t *const p = buf + ZLRS_FRAME_HDR_LEN;
	switch (frame->type) {
	case ZLRS_PKT_SYNC:
		buf[0] |= (uint8_t)((frame->body.sync.profile << ZLRS_SYNC_PROFILE_SHIFT) |
				    (frame->body.sync.version << ZLRS_SYNC_VERSION_SHIFT));
		p[0] = frame->body.sync.hop_index;
		sys_put_le32(frame->body.sync.nonce_leader, p + 1U);
		sys_put_le32(frame->body.sync.frame_index, p + 5U);
		encode_band_plan(p + 9U, &frame->body.sync.band_plan);
		break;
	case ZLRS_PKT_PAIR_REQ:
		memcpy(p, frame->body.pair_req.dev_a, ZLRS_DEV_ID_LEN);
		sys_put_le32(frame->body.pair_req.nonce_a, p + 4U);
		break;
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM: {
		const zlrs_pair_frame_t *const pair = &frame->body.pair;
		memcpy(p, pair->dev_a, ZLRS_DEV_ID_LEN);
		memcpy(p + 4U, pair->dev_b, ZLRS_DEV_ID_LEN);
		sys_put_le32(pair->nonce_a, p + 8U);
		sys_put_le32(pair->nonce_b, p + 12U);
		break;
	}
	case ZLRS_PKT_SESS_RESP:
		sys_put_le32(frame->body.sess_resp.nonce_leader, p);
		sys_put_le32(frame->body.sess_resp.nonce_follower, p + 4U);
		break;
	case ZLRS_PKT_SESS_CFM:
		sys_put_le32(frame->body.sess_cfm.nonce_follower, p);
		p[4] = frame->body.sess_cfm.hop_index;
		sys_put_le32(frame->body.sess_cfm.frame_index, p + 5U);
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
	    sys_get_le32(buf + 1U) == 0U) {
		return false;
	}
	const uint16_t payload_len = len - ZLRS_FRAME_HDR_LEN - tag_len;
	if (!payload_len_valid(type, payload_len)) {
		return false;
	}
	const uint8_t *const p = buf + ZLRS_FRAME_HDR_LEN;
	*frame = (zlrs_frame_t){.type = type};
	frame->seq = sys_get_le32(buf + 1U);
	memcpy(frame->tag, p + payload_len, tag_len);
	switch (frame->type) {
	case ZLRS_PKT_DATA_L2F:
	case ZLRS_PKT_DATA_F2L:
		return zlrs_frame_decode_data(&frame->body.data, buf, len, tag_len);
	case ZLRS_PKT_SYNC:
		frame->body.sync.version = buf[0] >> ZLRS_SYNC_VERSION_SHIFT;
		frame->body.sync.profile = (buf[0] >> ZLRS_SYNC_PROFILE_SHIFT) & 1U;
		frame->body.sync.hop_index = p[0];
		frame->body.sync.nonce_leader = sys_get_le32(p + 1U);
		frame->body.sync.frame_index = sys_get_le32(p + 5U);
		if (!decode_band_plan(&frame->body.sync.band_plan, p + 9U)) {
			return false;
		}
		break;
	case ZLRS_PKT_PAIR_REQ:
		memcpy(frame->body.pair_req.dev_a, p, ZLRS_DEV_ID_LEN);
		frame->body.pair_req.nonce_a = sys_get_le32(p + 4U);
		break;
	case ZLRS_PKT_PAIR_RESP:
	case ZLRS_PKT_PAIR_CFM: {
		zlrs_pair_frame_t *const pair = &frame->body.pair;
		memcpy(pair->dev_a, p, ZLRS_DEV_ID_LEN);
		memcpy(pair->dev_b, p + 4U, ZLRS_DEV_ID_LEN);
		pair->nonce_a = sys_get_le32(p + 8U);
		pair->nonce_b = sys_get_le32(p + 12U);
		break;
	}
	case ZLRS_PKT_SESS_RESP:
		frame->body.sess_resp.nonce_leader = sys_get_le32(p);
		frame->body.sess_resp.nonce_follower = sys_get_le32(p + 4U);
		break;
	case ZLRS_PKT_SESS_CFM:
		frame->body.sess_cfm.nonce_follower = sys_get_le32(p);
		frame->body.sess_cfm.hop_index = p[4];
		frame->body.sess_cfm.frame_index = sys_get_le32(p + 5U);
		if (!decode_band_plan(&frame->body.sess_cfm.band_plan, p + 9U)) {
			return false;
		}
		break;
	}
	return true;
}
