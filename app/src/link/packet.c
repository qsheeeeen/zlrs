/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Single-owner packet FIFO and direct payload admission. */

static uint16_t packet_stride(uint16_t mtu)
{
	return (uint16_t)((ZLRS_DATA_HEADROOM + mtu + ZLRS_TAG_LEN + 3U) & ~3U);
}

static uint16_t packet_limit(uint16_t mtu)
{
	if (mtu == 0U || mtu > ZLRS_DATA_MAX_LEN) {
		return 0U;
	}
	const uint16_t count = ZLRS_LINK_TX_STORAGE_LEN / packet_stride(mtu);
	return count < ZLRS_LINK_TX_PACKETS ? count : ZLRS_LINK_TX_PACKETS;
}

static uint16_t packet_index(const zlrs_link_t *link, uint16_t offset)
{
	return (uint16_t)((link->queue_head + offset) % link->tx_packet_limit);
}

uint8_t *link_packet_bytes(zlrs_link_t *link, uint16_t index)
{
	return &link->tx_storage[index * link->tx_stride];
}

void link_release_data(zlrs_link_t *link)
{
	__ASSERT_NO_MSG(link->tx_packet_count != 0U);
	zlrs_tx_packet_t *packet = &link->tx_packets[link->queue_head];
	link->queue_count -= packet->len;
	packet->len = 0U;
	packet->sealed = false;
	link->queue_head = packet_index(link, 1U);
	link->tx_packet_count--;
}

uint16_t zlrs_link_tx_space(const zlrs_link_t *link)
{
	const uint16_t mtu = link->cfg.data_mtu;
	const uint16_t limit = link->tx_packet_count ? link->tx_packet_limit : packet_limit(mtu);
	if (limit == 0U || (link->tx_packet_count && link->tx_pool_mtu != mtu)) {
		return 0U;
	}
	uint32_t space = (limit - link->tx_packet_count) * mtu;
	if (link->tx_packet_count != 0U) {
		const zlrs_tx_packet_t *tail =
			&link->tx_packets[packet_index(link, link->tx_packet_count - 1U)];
		if (!tail->sealed) {
			space += mtu - tail->len;
		}
	}
	const uint16_t budget = ZLRS_LINK_TX_QUEUE_LEN - link->queue_count;
	return space < budget ? (uint16_t)space : budget;
}

uint8_t *zlrs_link_tx_reserve(zlrs_link_t *link, uint16_t *capacity)
{
	*capacity = 0U;
	__ASSERT_NO_MSG(link->tx_claim_len == 0U);
	const uint16_t space = zlrs_link_tx_space(link);
	if (space == 0U) {
		return NULL;
	}
	if (link->tx_packet_count == 0U) {
		/* Empty pools can adopt a different MTU (diagnostics/tests). */
		link->tx_stride = packet_stride(link->cfg.data_mtu);
		link->tx_packet_limit = packet_limit(link->cfg.data_mtu);
		link->tx_pool_mtu = link->cfg.data_mtu;
		link->queue_head %= link->tx_packet_limit;
	}
	uint16_t index = packet_index(link, link->tx_packet_count);
	if (link->tx_packet_count != 0U) {
		const uint16_t tail = packet_index(link, link->tx_packet_count - 1U);
		if (!link->tx_packets[tail].sealed &&
		    link->tx_packets[tail].len < link->cfg.data_mtu) {
			index = tail;
		}
	}
	zlrs_tx_packet_t *packet = &link->tx_packets[index];
	const uint16_t room = link->cfg.data_mtu - packet->len;
	link->tx_claim_len = room < space ? room : space;
	link->tx_claim_index = index;
	*capacity = link->tx_claim_len;
	return link_packet_bytes(link, index) + ZLRS_DATA_HEADROOM + packet->len;
}

void zlrs_link_tx_commit(zlrs_link_t *link, uint16_t written)
{
	__ASSERT_NO_MSG(written <= link->tx_claim_len);
	if (written != 0U) {
		zlrs_tx_packet_t *packet = &link->tx_packets[link->tx_claim_index];
		if (packet->len == 0U) {
			link->tx_packet_count++;
		}
		packet->len += written;
		link->queue_count += written;
	}
	link->tx_claim_len = 0U;
}

uint16_t zlrs_link_write(zlrs_link_t *link, const uint8_t *data, uint16_t len)
{
	if (data == NULL) {
		return 0U;
	}
	uint16_t written = 0U;
	while (written < len) {
		uint16_t capacity;
		uint8_t *dst = zlrs_link_tx_reserve(link, &capacity);
		if (dst == NULL) {
			break;
		}
		const uint16_t take = len - written < capacity ? len - written : capacity;
		memcpy(dst, data + written, take);
		zlrs_link_tx_commit(link, take);
		written += take;
	}
	link->stats.data_drop_bytes += len - written;
	return written;
}
