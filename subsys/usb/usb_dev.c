/*
 * zlrs USB device stack: compose the devicetree CDCs into one device (three in the
 * application, two in the bootloader; same code, different strings and VID/PID). The product
 * names stay ZLRS / ZLRS MCUBOOT because the update tool finds ports by those names.
 *
 * In the device_next stack a `zephyr,cdc-acm-uart` node only says how many CDCs exist; it does
 * not bring the device up. CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT registers only the first
 * CDC, so the rest have no context and NULL-dereference on their first log write. We register all.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/dt-bindings/reset/rp2350_reset.h>
#include <zephyr/usb/usbd.h>

#include "usb_state.h"

LOG_MODULE_REGISTER(zlrs_usb, CONFIG_ZLRS_USB_LOG_LEVEL);

USBD_DEVICE_DEFINE(zlrs_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), CONFIG_ZLRS_USB_VID,
		   CONFIG_ZLRS_USB_PID);

static const struct reset_dt_spec usbctrl_reset = RESET_DT_SPEC_GET(DT_NODELABEL(zephyr_udc0));

USBD_DESC_LANG_DEFINE(zlrs_usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(zlrs_usb_mfr, CONFIG_ZLRS_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(zlrs_usb_product, CONFIG_ZLRS_USB_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(zlrs_usb_sn)));

USBD_DESC_CONFIG_DEFINE(zlrs_usb_fs_cfg_desc, "zlrs");
USBD_CONFIGURATION_DEFINE(zlrs_usb_fs_config, 0, CONFIG_ZLRS_USB_MAX_POWER, &zlrs_usb_fs_cfg_desc);

/* The high-speed descriptors exist for capable controllers: which one is used depends on caps */
USBD_DESC_CONFIG_DEFINE(zlrs_usb_hs_cfg_desc, "zlrs HS");
USBD_CONFIGURATION_DEFINE(zlrs_usb_hs_config, 0, CONFIG_ZLRS_USB_MAX_POWER, &zlrs_usb_hs_cfg_desc);

static int zlrs_usb_add_descriptors(void)
{
	static struct usbd_desc_node *const descs[] = {&zlrs_usb_lang, &zlrs_usb_mfr,
						       &zlrs_usb_product,
						       IF_ENABLED(CONFIG_HWINFO, (&zlrs_usb_sn,)) };

	for (uint8_t i = 0U; i < ARRAY_SIZE(descs); i++) {
		const int err = usbd_add_descriptor(&zlrs_usbd, descs[i]);

		if (err != 0) {
			LOG_ERR("descriptor %u add failed (%d)", i, err);
			return err;
		}
	}

	return 0;
}

/* The class code belongs in the interface descriptor (CDC); device level stays 0 */
static void zlrs_usb_class_codes(const enum usbd_speed speed)
{
	const int err = usbd_device_set_code_triple(&zlrs_usbd, speed, 0U, 0U, 0U);

	if (err != 0) {
		/* Not fatal: the host still enumerates, the device-level class code is just not 0/0/0 */
		LOG_WRN("usb device code triple not set (%d)", err);
	}
}

static int zlrs_usb_init(void)
{
	enum usbd_speed speed = USBD_SPEED_FS;
	int err;

	/*
	 * Show the host a real disconnect first: the bootloader (MCUboot serial recovery) may already
	 * have brought this controller up and enumerated once. The UDC driver reset on enable is too
	 * quick for the host to notice, so the port names would stay with the bootloader and the
	 * application's CDCs would never appear. Holding reset for 100 ms forces a re-enumeration.
	 */
	if (device_is_ready(usbctrl_reset.dev)) {
		(void)reset_line_assert(usbctrl_reset.dev, usbctrl_reset.id);
		k_msleep(100);
		(void)reset_line_deassert(usbctrl_reset.dev, usbctrl_reset.id);
	}

	/*
	 * USB is optional (doc/06-firmware.md): on failure warn but carry on — a non-zero SYS_INIT
	 * stops the whole system, and a board that only needs UART or the air should survive it.
	 */
	err = zlrs_usb_add_descriptors();
	if (err != 0) {
		LOG_ERR("usb init failed at descriptors (%d); continuing without USB", err);
		return 0;
	}

	/*
	 * The speed follows the controller: RP2350 is full speed, HS only shows up where supported.
	 * Registration has to happen before usbd_init() or the configuration comes up empty.
	 */
	if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&zlrs_usbd) == USBD_SPEED_HS) {
		speed = USBD_SPEED_HS;

		err = usbd_add_configuration(&zlrs_usbd, speed, &zlrs_usb_hs_config);
	} else {
		err = usbd_add_configuration(&zlrs_usbd, speed, &zlrs_usb_fs_config);
	}
	if (err != 0) {
		LOG_ERR("usb configuration add failed (%d); continuing without USB", err);
		return 0;
	}

	/* The devicetree CDCs are instances in the usbd_class section: register them all at once */
	err = usbd_register_all_classes(&zlrs_usbd, speed, 1U, NULL);
	if (err != 0) {
		LOG_ERR("usb class registration failed (%d); continuing without USB", err);
		return 0;
	}

	zlrs_usb_class_codes(speed);

	err = usbd_msg_register_cb(&zlrs_usbd, zlrs_usb_message);
	if (err != 0) {
		LOG_ERR("usb message registration failed (%d); continuing without USB", err);
		return 0;
	}

	err = usbd_init(&zlrs_usbd);
	if (err != 0) {
		LOG_ERR("usbd init failed (%d); continuing without USB", err);
		return 0;
	}

	err = usbd_enable(&zlrs_usbd);
	if (err != 0) {
		LOG_ERR("usbd enable failed (%d); continuing without USB", err);
		return 0;
	}

	LOG_INF("usb device up: %s %s (%s)", CONFIG_ZLRS_USB_MANUFACTURER, CONFIG_ZLRS_USB_PRODUCT,
		speed == USBD_SPEED_HS ? "HS" : "FS");

	return 0;
}

SYS_INIT(zlrs_usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
