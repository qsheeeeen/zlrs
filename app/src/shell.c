/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/shell/shell.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include "serial.h"
#include "mgmt.h"
#include "boot.h"
#include "rf.h"
#ifdef PICO_RP2350
#include <pico/bootrom.h>
#endif

#if IS_ENABLED(CONFIG_ZLRS_APP_DEBUG_SHELL)
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	zlrs_rf_status_t link = {0};
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	(void)zbus_chan_read(&zlrs_rf_chan, &link, K_MSEC(10));
	const zlrs_region_t *region = zlrs_region_get(link.region);
	shell_print(sh, "role   : %s", link.role == ZLRS_ROLE_TX ? "TX" : "RX");
	shell_print(sh, "sub-G  : %s",
		    DT_PROP(DT_NODELABEL(zlrs_rf), band) == ZLRS_BAND_SUBG_LF ? "LF" : "HF");
	shell_print(sh, "region : %s", region != NULL ? region->name : "?");
	shell_print(sh, "link   : state %u, lq %u%%, paired %d", link.state, link.lq_percent,
		    link.paired);
	shell_print(sh, "data   : RF drop %u B", link.data_drop_bytes);
	return 0;
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

/*
 * Data port endpoint state, for bench debugging. Picking an endpoint bit us twice on this
 * board: first DTR (never reaches us over usbipd), then "host bytes seen". This dumps both.
 */
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
		shell_print(sh, "%-16s ready=%d active=%d rx_total=%u rx_dropped=%u",
			    states[i].name, states[i].ready, states[i].active, states[i].rx_total,
			    states[i].rx_dropped);
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
	zlrs_cmds, SHELL_CMD(status, NULL, "role / SKU / region / link state", cmd_status),
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
