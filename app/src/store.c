/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "store.h"

#include <string.h>

#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(zlrs_store, CONFIG_APP_LOG_LEVEL);

/* NVS record ids */
#define NVS_ID_ROOT_KEY 1U
#define NVS_ID_DEV_ID 2U
#define NVS_ID_FAILSAFE 4U
#define NVS_ID_BAUD 5U
#define NVS_ID_PAIRING 6U
#define NVS_ID_REGION 7U
#define NVS_ID_POWER 8U

#define NVS_SECTOR_SIZE 4096U

static struct nvs_fs fs = {
	.offset = PARTITION_OFFSET(storage_partition),
	.sector_size = NVS_SECTOR_SIZE,
	.sector_count = PARTITION_SIZE(storage_partition) / NVS_SECTOR_SIZE,
};

static bool ready;

int zlrs_store_init(void)
{
	int rc;

	fs.flash_device = PARTITION_DEVICE(storage_partition);
	if (fs.flash_device == NULL) {
		return -1;
	}

	rc = nvs_mount(&fs);
	if (rc != 0) {
		LOG_ERR("NVS mount failed (%d)", rc);
		return rc;
	}

	ready = true;

	return 0;
}

static int read_record(uint16_t id, void *out, size_t len)
{
	if (!ready) {
		return -1;
	}

	return nvs_read(&fs, id, out, len);
}

static int write_record(uint16_t id, const void *data, size_t len)
{
	int rc;

	if (!ready) {
		LOG_ERR("store not ready; write of id %u dropped", id);
		return -1;
	}

	rc = nvs_write(&fs, id, data, len);
	/* Success has two shapes: len bytes written, or 0 when the content was identical and flash
	 * untouched. A sign check alone would call the first a failure and mismatched lengths a success. */
	if (rc == 0 || rc == (ssize_t)len) {
		return 0;
	}

	LOG_ERR("nvs_write(id %u, len %u) returned %d: treating as failure", id,
		(unsigned int)len, rc);

	return (rc < 0) ? (int)rc : -EIO;
}

int zlrs_store_load_root_key(uint8_t out[ZLRS_KEY_LEN])
{
	return read_record(NVS_ID_ROOT_KEY, out, ZLRS_KEY_LEN);
}

int zlrs_store_load_dev_id(uint8_t out[ZLRS_DEV_ID_LEN])
{
	return read_record(NVS_ID_DEV_ID, out, ZLRS_DEV_ID_LEN);
}

int zlrs_store_load_pairing(zlrs_store_pairing_t *out)
{
	return read_record(NVS_ID_PAIRING, out, sizeof(*out));
}

int zlrs_store_load_failsafe(uint8_t out[ZLRS_CFG_FAILSAFE_LEN])
{
	return read_record(NVS_ID_FAILSAFE, out, ZLRS_CFG_FAILSAFE_LEN);
}

int zlrs_store_load_baud(uint32_t *out)
{
	return read_record(NVS_ID_BAUD, out, sizeof(*out));
}

int zlrs_store_save_root_key(const uint8_t key[ZLRS_KEY_LEN])
{
	return write_record(NVS_ID_ROOT_KEY, key, ZLRS_KEY_LEN);
}

int zlrs_store_save_dev_id(const uint8_t id[ZLRS_DEV_ID_LEN])
{
	return write_record(NVS_ID_DEV_ID, id, ZLRS_DEV_ID_LEN);
}

int zlrs_store_save_pairing(const zlrs_store_pairing_t *pairing)
{
	return write_record(NVS_ID_PAIRING, pairing, sizeof(*pairing));
}

int zlrs_store_save_failsafe(const uint8_t channels[ZLRS_CFG_FAILSAFE_LEN])
{
	return write_record(NVS_ID_FAILSAFE, channels, ZLRS_CFG_FAILSAFE_LEN);
}

int zlrs_store_save_baud(uint32_t baud)
{
	return write_record(NVS_ID_BAUD, &baud, sizeof(baud));
}

int zlrs_store_load_region(uint8_t *out)
{
	return read_record(NVS_ID_REGION, out, sizeof(*out));
}

int zlrs_store_load_power(int8_t *out)
{
	return read_record(NVS_ID_POWER, out, sizeof(*out));
}

int zlrs_store_save_region(uint8_t region)
{
	return write_record(NVS_ID_REGION, &region, sizeof(region));
}

int zlrs_store_save_power(int8_t dbm)
{
	return write_record(NVS_ID_POWER, &dbm, sizeof(dbm));
}

int zlrs_store_erase_all(void)
{
	static const uint16_t ids[] = {
		NVS_ID_ROOT_KEY, NVS_ID_DEV_ID, NVS_ID_FAILSAFE,
		NVS_ID_BAUD,     NVS_ID_PAIRING, NVS_ID_REGION, NVS_ID_POWER,
	};

	if (!ready) {
		return -1;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(ids); i++) {
		const int rc = nvs_delete(&fs, ids[i]);

		/* A missing record counts as success: factory reset is what removes it */
		if (rc != 0 && rc != -ENOENT) {
			LOG_ERR("nvs_delete(id %u) returned %d", ids[i], rc);
			return rc;
		}
	}

	return 0;
}

int zlrs_store_erase_pairing(void)
{
	int rc;

	if (!ready) {
		return -1;
	}

	rc = nvs_delete(&fs, NVS_ID_PAIRING);

	return (rc == 0 || rc == -ENOENT) ? 0 : rc;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}

	return -1;
}

int zlrs_store_parse_hex_key(const char *hex, uint8_t out[ZLRS_KEY_LEN])
{
	if (strlen(hex) != ZLRS_KEY_LEN * 2U) {
		return -1;
	}

	for (size_t i = 0; i < ZLRS_KEY_LEN; i++) {
		const int hi = hex_nibble(hex[i * 2U]);
		const int lo = hex_nibble(hex[i * 2U + 1U]);

		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}

	return 0;
}
