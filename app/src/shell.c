/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/shell/shell.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include "serial.h"
#include "mgmt.h"
#include "boot.h"
#include "rf.h"
#include "platform/temperature.h"
#ifdef PICO_RP2350
#include <pico/bootrom.h>
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
static void status_temperature(const struct shell *sh, const char *name, int16_t value)
{
	const int magnitude = value < 0 ? -value : value;

	shell_print(sh, "%s: %s%d.%02d C", name, value < 0 ? "-" : "", magnitude / 100,
		    magnitude % 100);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	zlrs_rf_status_t link = {0};
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	(void)zbus_chan_read(&zlrs_rf_chan, &link, K_MSEC(10));
	const zlrs_region_t *region = zlrs_region_get(link.region);
	shell_print(sh, "role   : %s", link.role == ZLRS_ROLE_LEADER ? "Leader" : "Follower");
	shell_print(sh, "sub-G  : %s",
		    DT_PROP(DT_NODELABEL(zlrs_rf), band) == ZLRS_BAND_SUBG_LF ? "LF" : "HF");
	shell_print(sh, "region : %s", region != NULL ? region->name : "?");
	shell_print(sh, "band   : %s", link.data_band == ZLRS_DATA_BAND_SUBG ? "sub-G" : "2.4 GHz");
	shell_print(sh, "switch : %s, capture %d, planned %d, count %u",
		    link.band_mode == ZLRS_DATA_BAND_AUTO ? "auto" : "fixed", link.band_acquiring,
		    link.band_planned, link.band_switches);
	shell_print(sh, "link   : state %u, lq %u%%, paired %d", link.state, link.lq_percent,
		    link.paired);
	if (link.rssi_dbm != 0) {
		shell_print(sh, "signal : RSSI %d dBm", link.rssi_dbm);
	} else {
		shell_print(sh, "signal : RSSI unavailable");
	}
	if (link.peer_rssi_dbm != 0) {
		shell_print(sh, "peer   : RSSI %d dBm, lq %u%%", link.peer_rssi_dbm,
			    link.peer_lq_percent);
	} else {
		shell_print(sh, "peer   : feedback unavailable");
	}
	shell_print(sh, "power  : %d dBm, range %d..%d dBm", link.tx_power_dbm,
		    link.power_floor_dbm, link.power_ceiling_dbm);
	shell_print(sh, "control: %s", link.power_fixed ? "debug fixed" : "automatic");
	int16_t temperature;
	if (zlrs_temperature_read(&temperature) == 0) {
		const int magnitude = temperature < 0 ? -temperature : temperature;
		shell_print(sh, "temp   : MCU %s%d.%02d C", temperature < 0 ? "-" : "",
			    magnitude / 100, magnitude % 100);
	} else {
		shell_print(sh, "temp   : MCU unavailable");
	}
	shell_print(sh, "hop    : %u.%03u MHz, index %u, frame %u us", link.frequency_hz / 1000000U,
		    (link.frequency_hz % 1000000U) / 1000U, link.hop_index, link.frame_period_us);
	shell_print(sh, "frames : TX %u, RX %u, retry %u", link.tx_frames, link.rx_frames,
		    link.tx_retries);
	shell_print(sh, "errors : CRC %u, tag %u, radio %u, missed slots %u", link.rx_crc_errors,
		    link.rx_bad_tag, link.radio_fail, link.missed_slots);
	shell_print(sh, "data   : RF drop %u B", link.data_drop_bytes);
	const zlrs_lr2021_status_t *radio = &link.radio;
	if (radio->valid & ZLRS_LR2021_STATUS_TEMP) {
		status_temperature(sh, "lr temp", radio->temperature_centi_c);
	} else {
		shell_print(sh, "lr temp: unavailable");
	}
	if (radio->valid & ZLRS_LR2021_STATUS_XOSC_TEMP) {
		status_temperature(sh, "lr xosc", radio->xosc_temperature_centi_c);
	} else {
		shell_print(sh, "lr xosc: unavailable");
	}
	if (radio->valid & ZLRS_LR2021_STATUS_VBAT) {
		shell_print(sh, "lr vbat: %u mV", radio->vbat_mv);
	} else {
		shell_print(sh, "lr vbat: unavailable");
	}
	if (radio->valid & ZLRS_LR2021_STATUS_VERSION) {
		shell_print(sh, "lr ver : major 0x%02x, minor 0x%02x", radio->version_major,
			    radio->version_minor);
	} else {
		shell_print(sh, "lr ver : unavailable");
	}
	if (radio->valid & ZLRS_LR2021_STATUS_STATE) {
		shell_print(sh, "lr stat: mode %u, command %u, reset %u, IRQ 0x%08x",
			    radio->chip_mode, radio->command_status, radio->reset_status,
			    radio->irq);
	} else {
		shell_print(sh, "lr stat: unavailable");
	}
	if (radio->valid & ZLRS_LR2021_STATUS_ERRORS) {
		shell_print(sh, "lr errs: 0x%04x", radio->errors);
	} else {
		shell_print(sh, "lr errs: unavailable");
	}
	if (radio->valid != 0U) {
		shell_print(sh, "lr age : %u ms (boot/management snapshot)",
			    k_uptime_get_32() - radio->sampled_ms);
	}
	return 0;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const bool automatic = strcmp(argv[1], "auto") == 0;
	char *end;
	const long dbm = automatic ? 0 : strtol(argv[1], &end, 10);
	if (!automatic && (end == argv[1] || *end != '\0' || dbm < -128 || dbm > 127)) {
		return -EINVAL;
	}
	const int rc = zlrs_rf_power_request(automatic, (int)dbm);
	if (rc == 0) {
		shell_print(sh, "power request queued: %s", automatic ? "automatic" : argv[1]);
	} else {
		shell_error(sh,
			    "power request rejected (%d); fixed power requires a connected link "
			    "and an in-range value",
			    rc);
	}
	return rc;
}

#if DT_NODE_EXISTS(DT_ALIAS(zlrs_mgmt_cdc))
static int cmd_mgmt(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zlrs_mgmt_request_session();
	shell_print(
		sh,
		"management mode requested: the management CDC speaks the protocol, EXIT returns");

	return 0;
}
#endif

/* Data endpoint readiness, selection and queue state for bench observation. */
static int cmd_ports(const struct shell *sh, size_t argc, char **argv)
{
	zlrs_serial_state_t states[2];
	const size_t count = zlrs_serial_state(states, ARRAY_SIZE(states));

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (count == 0U) {
		shell_print(sh, "data port has no usable endpoint");
		return 0;
	}

	for (size_t i = 0U; i < count; i++) {
		shell_print(sh, "%-16s ready=%d active=%d rx_total=%u tx_total=%u rx_dropped=%u",
			    states[i].name, states[i].ready, states[i].active, states[i].rx_total,
			    states[i].tx_total, states[i].rx_dropped);
		shell_print(sh, "                 rx_queue=%u tx_queue=%u rx_paused=%d",
			    states[i].rx_queued, states[i].tx_queued, states[i].rx_paused);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
static int cmd_recovery(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "rebooting into MCUboot serial recovery");
	(void)zlrs_boot_recovery(); /* does not return */

	return 0;
}
#endif

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
/*
 * Debug helper: dump the image state as MCUboot sees it. First thing to look at when an
 * upgrade or rollback misbehaves; confirmed=1 means MCUboot has pinned this build for good.
 */
static void mcuboot_dump_slot(const struct shell *sh, const char *name, uint8_t area_id)
{
	const struct flash_area *fa;
	static uint8_t sector[4096];

	if (flash_area_open(area_id, &fa) != 0) {
		shell_print(sh, "%s: cannot open flash area %u", name, area_id);
		return;
	}

	/*
	 * MCUboot's trailer lives in the last sector of the slot. Scan for non-0xFF bytes so that the
	 * positions and values show up, which makes magic / image_ok / copy_done obvious.
	 */
	/* State may not sit in the last sector: scan 16 sectors back, list the non-0xFF bytes */
	shell_print(sh, "%s: non-0xFF bytes in the last 64 KB (offset from the slot end):", name);

	uint8_t shown = 0U;

	for (uint32_t back = 1U; back <= 16U && shown < 12U; back++) {
		const uint32_t off = fa->fa_size - back * sizeof(sector);

		if (flash_area_read(fa, off, sector, sizeof(sector)) != 0) {
			shell_print(sh, "  read at 0x%05x failed", off);
			break;
		}

		for (size_t i = 0U; i < sizeof(sector) && shown < 12U; i++) {
			if (sector[i] != 0xFFU) {
				shell_print(sh, "  -0x%05x = 0x%02x", back * 4096U - i, sector[i]);
				shown++;
			}
		}
	}

	if (shown == 0U) {
		shell_print(sh, "  (no writes found in these 64 KB)");
	}

	flash_area_close(fa);
}

static int cmd_mcuboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "active slot : %u", boot_fetch_active_slot());
	/* VTOR points at the slot really running (cross-check against MCUboot's active slot) */
	shell_print(sh, "VTOR        : 0x%08x (slot0=0x%08x, slot1=0x%08x)",
		    (unsigned int)SCB->VTOR,
		    (unsigned int)(0x10000000 + PARTITION_OFFSET(slot0_partition)),
		    (unsigned int)(0x10000000 + PARTITION_OFFSET(slot1_partition)));
	shell_print(sh, "confirmed   : %d", (int)boot_is_img_confirmed());
	shell_print(sh, "swap type   : %d (0=none 1=test 2=revert 3=fail 4=permanent)",
		    (int)mcuboot_swap_type());
	mcuboot_dump_slot(sh, "slot0", PARTITION_ID(slot0_partition));
	mcuboot_dump_slot(sh, "slot1", PARTITION_ID(slot1_partition));

	return 0;
}
#endif

#ifdef PICO_RP2350
static int cmd_bootsel(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "rebooting into the RP2350 ROM bootloader (USB drive)");
	k_msleep(50);
	reset_usb_boot(0, 0); /* does not return */

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	zlrs_cmds,
	SHELL_CMD(status, NULL, "link, MCU temperature, LR2021 snapshot and error counters",
		  cmd_status),
	SHELL_CMD_ARG(power, NULL, "temporary fixed dBm while connected, or auto", cmd_power, 2, 0),
	SHELL_CMD(ports, NULL, "state of both data port endpoints", cmd_ports),
#if DT_NODE_EXISTS(DT_ALIAS(zlrs_mgmt_cdc))
	SHELL_CMD(mgmt, NULL, "enter management mode (EXIT returns)", cmd_mgmt),
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	SHELL_CMD(recovery, NULL, "reboot into MCUboot serial recovery", cmd_recovery),
#endif
#ifdef PICO_RP2350
	SHELL_CMD(bootsel, NULL, "reboot into the RP2350 ROM bootloader (USB drive)", cmd_bootsel),
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	SHELL_CMD(mcuboot, NULL, "image state as MCUboot sees it (debug)", cmd_mcuboot),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(zlrs, &zlrs_cmds, "zlrs debug commands", NULL);
#endif /* CONFIG_ZLRS_APP_DEBUG_SHELL */
