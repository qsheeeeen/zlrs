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
static void status_temperature(const struct shell *sh, const char *name, bool valid, int16_t value)
{
	const int magnitude = value < 0 ? -value : value;

	if (valid) {
		shell_print(sh, "  %-20s : %s%d.%02d C", name, value < 0 ? "-" : "",
			    magnitude / 100, magnitude % 100);
	} else {
		shell_print(sh, "  %-20s : unavailable", name);
	}
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	zlrs_rf_status_t link;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	const int rc = zbus_chan_read(&zlrs_rf_chan, &link, K_MSEC(10));
	if (rc != 0) {
		shell_error(sh, "RF status unavailable (%d)", rc);
		return rc;
	}
	const zlrs_region_t *region = zlrs_region_get(link.region);
	const zlrs_lr2021_status_t *radio = &link.radio;
	static const char *const states[] = {"unconfigured", "unpaired", "pairing", "searching",
					     "connected"};

	shell_print(sh, "ZLRS Status\n\nLink");
	shell_print(sh, "  %-20s : %s", "Role",
		    link.role == ZLRS_ROLE_LEADER ? "Leader" : "Follower");
	shell_print(sh, "  %-20s : %s", "State",
		    (unsigned int)link.state < ARRAY_SIZE(states) ? states[link.state] : "unknown");
	shell_print(sh, "  %-20s : %s", "Paired", link.paired ? "yes" : "no");
	shell_print(sh, "  %-20s : %s", "Session", link.session_valid ? "active" : "inactive");

	shell_print(sh, "\nRadio");
	shell_print(sh, "  %-20s : %s", "Band mode",
		    link.band_mode == ZLRS_DATA_BAND_AUTO ? "auto" : "fixed");
	shell_print(sh, "  %-20s : %s", "Active band",
		    link.data_band == ZLRS_DATA_BAND_SUBG ? "sub-GHz" : "2.4 GHz");
	shell_print(sh, "  %-20s : %s", "Modulation", zlrs_mod_phy_name(link.data_phy));
	shell_print(sh, "  %-20s : %u.%03u MHz", "Frequency", link.frequency_hz / 1000000U,
		    (link.frequency_hz % 1000000U) / 1000U);
	shell_print(sh, "  %-20s : %u", "Hop index", link.hop_index);
	shell_print(sh, "  %-20s : %u.%03u ms", "Frame period", link.frame_period_us / 1000U,
		    link.frame_period_us % 1000U);
	shell_print(sh, "  %-20s : %s", "Capture", link.band_acquiring ? "searching" : "ready");
	shell_print(sh, "  %-20s : %s", "Transition", link.band_planned ? "scheduled" : "none");
	shell_print(sh, "  %-20s : %u", "Switch count", link.band_switches);

	shell_print(sh, "\nSignal / Power");
	if (link.rssi_dbm != 0) {
		shell_print(sh, "  %-20s : %d dBm", "Local RSSI", link.rssi_dbm);
	} else {
		shell_print(sh, "  %-20s : unavailable", "Local RSSI");
	}
	shell_print(sh, "  %-20s : %u%%", "Local LQ", link.lq_percent);
	if (link.peer_rssi_dbm != 0) {
		shell_print(sh, "  %-20s : %d dBm", "Peer RSSI", link.peer_rssi_dbm);
		shell_print(sh, "  %-20s : %u%%", "Peer LQ", link.peer_lq_percent);
	} else {
		shell_print(sh, "  %-20s : unavailable", "Peer RSSI");
		shell_print(sh, "  %-20s : unavailable", "Peer LQ");
	}
	shell_print(sh, "  %-20s : %d dBm", "TX power", link.tx_power_dbm);
	shell_print(sh, "  %-20s : %d..%d dBm", "TX range", link.power_floor_dbm,
		    link.power_ceiling_dbm);
	shell_print(sh, "  %-20s : %s", "Power control",
		    link.power_fixed ? "debug fixed" : "automatic");

	shell_print(sh, "\nTraffic / Errors");
	shell_print(sh, "  %-20s : TX %u, RX %u", "RF frames", link.tx_frames, link.rx_frames);
	shell_print(sh, "  %-20s : %u", "ARQ retries", link.tx_retries);
	shell_print(sh, "  %-20s : %u", "CRC errors", link.rx_crc_errors);
	shell_print(sh, "  %-20s : %u", "Auth errors", link.rx_bad_tag);
	shell_print(sh, "  %-20s : %u", "Radio failures", link.radio_fail);
	shell_print(sh, "  %-20s : %u", "Missed slots", link.missed_slots);
	shell_print(sh, "  %-20s : %u bytes", "Dropped data", link.data_drop_bytes);

	shell_print(sh, "\nDevice / Configuration");
	shell_print(sh, "  %-20s : %s", "Hardware",
		    DT_PROP(DT_NODELABEL(zlrs_rf), band) == ZLRS_BAND_SUBG_LF ? "LF (470/433 MHz)"
									      : "HF (868/915 MHz)");
	shell_print(sh, "  %-20s : %s", "Region", region != NULL ? region->name : "unknown");
	shell_print(sh, "  %-20s : P%u", "Security profile", link.profile);
	shell_print(sh, "  %-20s : %02x%02x%02x%02x", "Device ID", link.dev_id[0], link.dev_id[1],
		    link.dev_id[2], link.dev_id[3]);
	shell_print(sh, "  %-20s : %02x%02x%02x%02x", "Peer ID", link.peer_id[0], link.peer_id[1],
		    link.peer_id[2], link.peer_id[3]);
	shell_print(sh, "  %-20s : %s", "Root key", link.configured ? "configured" : "missing");

	shell_print(sh, "\nSensors");
	int16_t temperature = 0;
	const bool temperature_valid = zlrs_temperature_read(&temperature) == 0;
	status_temperature(sh, "MCU chip temp", temperature_valid, temperature);
	status_temperature(sh, "LR2021 chip temp", radio->valid & ZLRS_LR2021_STATUS_TEMP,
			   radio->temperature_centi_c);
	status_temperature(sh, "LR2021 XOSC temp", radio->valid & ZLRS_LR2021_STATUS_XOSC_TEMP,
			   radio->xosc_temperature_centi_c);
	if (radio->valid & ZLRS_LR2021_STATUS_VBAT) {
		shell_print(sh, "  %-20s : %u.%03u V (%u mV)", "LR2021 supply",
			    radio->vbat_mv / 1000U, radio->vbat_mv % 1000U, radio->vbat_mv);
	} else {
		shell_print(sh, "  %-20s : unavailable", "LR2021 supply");
	}
	if (radio->valid != 0U) {
		shell_print(sh, "  %-20s : %u ms", "LR2021 sample age",
			    k_uptime_get_32() - radio->sampled_ms);
	}

	shell_print(sh, "\nLR2021 snapshot");
	if (radio->valid & ZLRS_LR2021_STATUS_VERSION) {
		shell_print(sh, "  %-20s : major 0x%02x, minor 0x%02x", "Chip version",
			    radio->version_major, radio->version_minor);
	} else {
		shell_print(sh, "  %-20s : unavailable", "Chip version");
	}
	if (radio->valid & ZLRS_LR2021_STATUS_STATE) {
		shell_print(sh, "  %-20s : %u", "Chip mode", radio->chip_mode);
		shell_print(sh, "  %-20s : %u", "Last command", radio->command_status);
		shell_print(sh, "  %-20s : %u", "Reset source", radio->reset_status);
		shell_print(sh, "  %-20s : 0x%08x", "IRQ status", radio->irq);
	} else {
		shell_print(sh, "  %-20s : unavailable", "Chip mode");
		shell_print(sh, "  %-20s : unavailable", "Last command");
		shell_print(sh, "  %-20s : unavailable", "Reset source");
		shell_print(sh, "  %-20s : unavailable", "IRQ status");
	}
	if (radio->valid & ZLRS_LR2021_STATUS_ERRORS) {
		shell_print(sh, "  %-20s : 0x%04x", "Chip errors", radio->errors);
	} else {
		shell_print(sh, "  %-20s : unavailable", "Chip errors");
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

static int cmd_phy(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	char *end;
	const unsigned long phy = strtoul(argv[1], &end, 10);
	if (argv[1][0] == '\0' || *end != '\0' || phy >= ZLRS_PHY_AUTO_COUNT) {
		shell_error(sh, "profile must be 0..4");
		return -EINVAL;
	}
	const int rc = zlrs_rf_phy_request(phy);
	if (rc != 0) {
		shell_error(sh, "profile request requires an automatic-mode Leader (%d)", rc);
		return rc;
	}
	shell_print(sh, "queued profile %lu: %s", phy, zlrs_mod_phy_name(phy));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	zlrs_cmds,
	SHELL_CMD(status, NULL, "link, MCU temperature, LR2021 snapshot and error counters",
		  cmd_status),
	SHELL_CMD_ARG(power, NULL, "temporary fixed dBm while connected, or auto", cmd_power, 2, 0),
	SHELL_CMD_ARG(phy, NULL, "negotiate one 2.4 GHz profile, 0..4 (Leader, auto)", cmd_phy, 2,
		      0),
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
