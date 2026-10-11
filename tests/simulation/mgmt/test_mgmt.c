/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>
#include <zephyr/kvss/nvs.h>
#include "mgmt.h"
#include "log.h"
#include "persist.h"
#include "rf.h"
#include "zlrs/mgmt/mgmt.h"
#include "zlrs/proto/crypto.h"

static zlrs_rf_status_t rf = {.role = ZLRS_ROLE_TX, .profile = ZLRS_PROFILE_P1};
static unsigned int credential_applies, pairs, confirms;
static int confirm_rc;
static bool fail_write;
ssize_t __real_nvs_write(struct nvs_fs *, uint16_t, const void *, size_t);
ssize_t __wrap_nvs_write(struct nvs_fs *fs, uint16_t id, const void *bytes, size_t len)
{
	return fail_write ? -EIO : __real_nvs_write(fs, id, bytes, len);
}
zlrs_rf_status_t zlrs_rf_status(void)
{
	return rf;
}
void zlrs_rf_credentials(const zlrs_persist_state_t *s)
{
	credential_applies++;
	rf.configured = s->configured;
	rf.paired = s->paired;
	memcpy(rf.dev_id, s->dev_id, sizeof(rf.dev_id));
}
int zlrs_rf_pair_start(void)
{
	pairs++;
	return rf.configured ? 0 : -ENOTCONN;
}
int zlrs_boot_confirm(void)
{
	confirms++;
	return confirm_rc;
}
int zlrs_boot_recovery(void)
{
	return -EIO;
}
void zlrs_watchdog_feed(void)
{
}
void zlrs_watchdog_disarm(void)
{
}
void zlrs_watchdog_arm(void)
{
}
void zlrs_serial_publish_state(void)
{
}
void zlrs_serial_discard_rx(void)
{
}
int zlrs_log_info(zlrs_log_info_t *out)
{
	*out = (zlrs_log_info_t){.count = 1, .newest = 0, .num = {7}, .size = {5}};
	return 0;
}
int zlrs_log_read(uint8_t index, uint32_t offset, uint8_t len, uint8_t *out, uint8_t *got)
{
	if (index != 0 || len == 0) {
		return -EINVAL;
	}
	if (offset > 5) {
		return -EIO;
	}
	*got = MIN(len, 5U - offset);
	memcpy(out, "hello" + offset, *got);
	return 0;
}
static zlrs_mgmt_parser_t request(uint8_t cmd, const uint8_t *body, uint8_t size)
{
	uint8_t frame[ZLRS_MGMT_FRAME_MAX_LEN];
	const uint8_t n = zlrs_mgmt_command(cmd, body, size, frame, sizeof(frame));
	zlrs_mgmt_parser_t parser;
	zlrs_mgmt_parser_init(&parser);
	int result = 0;
	for (uint8_t i = 0; i < n; i++) {
		result = zlrs_mgmt_feed(&parser, frame[i]);
	}
	zassert_equal(result, 1);
	return parser;
}
static void ack(uint8_t cmd, const uint8_t *body, uint8_t size, uint8_t status)
{
	const zlrs_mgmt_parser_t p = request(cmd, body, size);
	zassert_equal(p.cmd, ZLRS_MGMT_CMD_ACK);
	zassert_equal(p.payload_len, 2);
	zassert_equal(ZLRS_MGMT_PAYLOAD(&p)[0], cmd);
	zassert_equal(ZLRS_MGMT_PAYLOAD(&p)[1], status);
}
ZTEST(commands, test_device_commands)
{
	zassert_ok(zlrs_persist_init());
	ack(ZLRS_MGMT_CMD_ENTER, NULL, 0, 0);
	ack(ZLRS_MGMT_CMD_PAIR, NULL, 0, ZLRS_MGMT_E_STATE);
	const uint8_t baud[] = {0, 0x84, 3, 0};
	ack(ZLRS_MGMT_CMD_SET_BAUD, baud, 4, 0);
	zassert_equal(zlrs_persist_state()->settings.data_baud, 230400);
	ack(ZLRS_MGMT_CMD_SET_BAUD, baud, 3, ZLRS_MGMT_E_PARAM);
	const uint8_t invalid = 255, subg = ZLRS_DATA_BAND_SUBG, role = ZLRS_ROLE_RX;
	ack(ZLRS_MGMT_CMD_SET_ROLE, &invalid, 1, ZLRS_MGMT_E_PARAM);
	ack(ZLRS_MGMT_CMD_SET_REGION, &invalid, 1, ZLRS_MGMT_E_PARAM);
	ack(ZLRS_MGMT_CMD_SET_BAND, &subg, 1, 0);
	ack(ZLRS_MGMT_CMD_SET_ROLE, &role, 1, 0);
	const uint8_t unsupported = ZLRS_PROFILE_P2;
	ack(ZLRS_MGMT_CMD_SET_PROFILE, &unsupported, 1, ZLRS_MGMT_E_PARAM);
	const uint8_t power = (uint8_t)-9;
	fail_write = true;
	ack(ZLRS_MGMT_CMD_SET_POWER, &power, 1, ZLRS_MGMT_E_STORE);
	zassert_equal(zlrs_persist_state()->settings.power_dbm, -10);
	fail_write = false;
	ack(ZLRS_MGMT_CMD_SET_POWER, &power, 1, 0);
	uint8_t hex[64];
	memset(hex, 'a', sizeof(hex));
	ack(ZLRS_MGMT_CMD_SET_KEY_HEX, hex, 63, ZLRS_MGMT_E_PARAM);
	hex[0] = 'z';
	ack(ZLRS_MGMT_CMD_SET_KEY_HEX, hex, 64, ZLRS_MGMT_E_PARAM);
	hex[0] = 'a';
	ack(ZLRS_MGMT_CMD_SET_KEY_HEX, hex, 64, 0);
	zassert_equal(zlrs_persist_state()->root_key[0], 0xAA);
	ack(ZLRS_MGMT_CMD_SET_PHRASE, NULL, 0, ZLRS_MGMT_E_PARAM);
	const uint8_t phrase[] = "mgmt-native";
	ack(ZLRS_MGMT_CMD_SET_PHRASE, phrase, sizeof(phrase) - 1, 0);
	uint8_t expected[32];
	zlrs_root_key_from_phrase((const char *)phrase, expected);
	zassert_mem_equal(zlrs_persist_state()->root_key, expected, sizeof(expected));
	zassert_equal(credential_applies, 2);
	ack(ZLRS_MGMT_CMD_PAIR, NULL, 0, 0);
	zassert_equal(pairs, 2);
	zlrs_mgmt_parser_t p = request(ZLRS_MGMT_CMD_STATUS, NULL, 0);
	zlrs_mgmt_status_t status;
	zassert_true(zlrs_mgmt_status_decode(ZLRS_MGMT_PAYLOAD(&p), p.payload_len, &status));
	zassert_equal(p.payload_len, 21);
	zassert_equal(status.role, ZLRS_ROLE_TX); /* Running role; saved role is RX until reboot. */
	zassert_equal(status.data_baud, 230400);
	zassert_equal(status.tx_power_dbm, -9);
	p = request(ZLRS_MGMT_CMD_BUILD, NULL, 0);
	zassert_equal(p.cmd, ZLRS_MGMT_CMD_BUILD_RESP);
	confirm_rc = -EIO;
	ack(ZLRS_MGMT_CMD_CONFIRM, NULL, 0, ZLRS_MGMT_E_STORE);
	confirm_rc = 0;
	ack(ZLRS_MGMT_CMD_CONFIRM, NULL, 0, 0);
	zassert_equal(confirms, 2);
	ack(ZLRS_MGMT_CMD_RECOVERY, NULL, 0, ZLRS_MGMT_E_STATE);
	p = request(ZLRS_MGMT_CMD_LOG_INFO, NULL, 0);
	zassert_equal(p.cmd, ZLRS_MGMT_CMD_LOG_INFO_RESP);
	zassert_equal(p.payload_len, 8);
	zassert_equal(ZLRS_MGMT_PAYLOAD(&p)[2], 7);
	const uint8_t read[] = {0, 1, 0, 0, 0, 4};
	p = request(ZLRS_MGMT_CMD_LOG_READ, read, 6);
	zassert_equal(p.cmd, ZLRS_MGMT_CMD_LOG_DATA_RESP);
	zassert_mem_equal(ZLRS_MGMT_PAYLOAD(&p) + 5, "ello", 4);
	ack(ZLRS_MGMT_CMD_LOG_READ, read, 5, ZLRS_MGMT_E_PARAM);
	const uint8_t big[] = {0, 0, 0, 0, 0, 81};
	ack(ZLRS_MGMT_CMD_LOG_READ, big, 6, ZLRS_MGMT_E_PARAM);
	const uint8_t io_error[] = {0, 6, 0, 0, 0, 4};
	ack(ZLRS_MGMT_CMD_LOG_READ, io_error, 6, ZLRS_MGMT_E_STATE);
	const uint8_t wrong_index[] = {1, 0, 0, 0, 0, 4};
	ack(ZLRS_MGMT_CMD_LOG_READ, wrong_index, 6, ZLRS_MGMT_E_PARAM);
	ack(ZLRS_MGMT_CMD_NEW_DEV_ID, read, 1, ZLRS_MGMT_E_PARAM);
	uint8_t old_id[ZLRS_DEV_ID_LEN];
	memcpy(old_id, zlrs_persist_state()->dev_id, sizeof(old_id));
	fail_write = true;
	ack(ZLRS_MGMT_CMD_NEW_DEV_ID, NULL, 0, ZLRS_MGMT_E_STORE);
	zassert_mem_equal(zlrs_persist_state()->dev_id, old_id, sizeof(old_id));
	zassert_mem_equal(rf.dev_id, old_id, sizeof(old_id));
	fail_write = false;
	const unsigned int before = credential_applies;
	ack(ZLRS_MGMT_CMD_NEW_DEV_ID, NULL, 0, 0);
	zassert_equal(credential_applies, before + 1);
	ack(ZLRS_MGMT_CMD_ERASE_PAIR, NULL, 0, 0);
	ack(ZLRS_MGMT_CMD_FACTORY_RESET, NULL, 0, 0);
	zassert_false(rf.configured);
	ack(0x7e, NULL, 0, ZLRS_MGMT_E_UNKNOWN);
	ack(ZLRS_MGMT_CMD_EXIT, NULL, 0, 0);
}
ZTEST_SUITE(commands, NULL, NULL, NULL, NULL, NULL);
