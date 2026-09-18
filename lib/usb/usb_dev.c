/*
 * zlrs 的 USB 设备栈：把 devicetree 里的 CDC 组成复合设备（应用三块、bootloader
 * 两块，代码同一份，只有字符串与 VID/PID 不同）。产品名要留成 ZLRS / ZLRS MCUBOOT，
 * 升级脚本按 /dev/serial/by-id 里的名字找端口。
 *
 * device_next 栈里 `zephyr,cdc-acm-uart` 节点只表达"要几块 CDC"，不会让设备上线；
 * 而 CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT 只注册第一块 CDC，其余没有 context，
 * 第一次往它们写日志就 NULL 解引用。所以这里自己注册全部 CDC。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(zlrs_usb, CONFIG_ZLRS_USB_LOG_LEVEL);

USBD_DEVICE_DEFINE(zlrs_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_ZLRS_USB_VID, CONFIG_ZLRS_USB_PID);

USBD_DESC_LANG_DEFINE(zlrs_usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(zlrs_usb_mfr, CONFIG_ZLRS_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(zlrs_usb_product, CONFIG_ZLRS_USB_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(zlrs_usb_sn)));

USBD_DESC_CONFIG_DEFINE(zlrs_usb_fs_cfg_desc, "zlrs");
USBD_CONFIGURATION_DEFINE(zlrs_usb_fs_config, 0, CONFIG_ZLRS_USB_MAX_POWER,
			  &zlrs_usb_fs_cfg_desc);

/* 高速那份只在支持它的控制器上用：定义先留着，选哪份由 usbd_caps_speed() 定 */
USBD_DESC_CONFIG_DEFINE(zlrs_usb_hs_cfg_desc, "zlrs HS");
USBD_CONFIGURATION_DEFINE(zlrs_usb_hs_config, 0, CONFIG_ZLRS_USB_MAX_POWER,
			  &zlrs_usb_hs_cfg_desc);

static int zlrs_usb_add_descriptors(void)
{
	static struct usbd_desc_node *const descs[] = {
		&zlrs_usb_lang,
		&zlrs_usb_mfr,
		&zlrs_usb_product,
		IF_ENABLED(CONFIG_HWINFO, (&zlrs_usb_sn,))
	};

	for (uint8_t i = 0U; i < ARRAY_SIZE(descs); i++) {
		const int err = usbd_add_descriptor(&zlrs_usbd, descs[i]);

		if (err != 0) {
			LOG_ERR("descriptor %u add failed (%d)", i, err);
			return err;
		}
	}

	return 0;
}

/* 类码放接口描述符里（CDC 的接口级类码），设备级给 0 */
static void zlrs_usb_class_codes(const enum usbd_speed speed)
{
	const int err = usbd_device_set_code_triple(&zlrs_usbd, speed, 0U, 0U, 0U);

	if (err != 0) {
		/* 不致命：主机多半还是照常枚举，只是设备级类码不是 0/0/0 */
		LOG_WRN("usb device code triple not set (%d)", err);
	}
}

static int zlrs_usb_init(void)
{
	enum usbd_speed speed = USBD_SPEED_FS;
	int err;

	/*
	 * USB 可裁（doc/08-firmware.md 第 4 节）：失败只报警不住手——SYS_INIT 返回非零会
	 * 让 Zephyr 停住整机，而 USB 起不来不该拖死只能跑串口/空口的板子。
	 */
	err = zlrs_usb_add_descriptors();
	if (err != 0) {
		LOG_ERR("usb init failed at descriptors (%d); continuing without USB", err);
		return 0;
	}

	/*
	 * 速度按控制器的能力选：RP2350 是全速，HS 只在支持它的控制器上出现。
	 * 注册必须放在 usbd_init() 之前，否则配置是空的。
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

	/* devicetree 里那几块 CDC 都是 usbd_class 段里的实例，一次全注册 */
	err = usbd_register_all_classes(&zlrs_usbd, speed, 1U, NULL);
	if (err != 0) {
		LOG_ERR("usb class registration failed (%d); continuing without USB", err);
		return 0;
	}

	zlrs_usb_class_codes(speed);

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

	LOG_INF("usb device up: %s %s (%s)", CONFIG_ZLRS_USB_MANUFACTURER,
		CONFIG_ZLRS_USB_PRODUCT, speed == USBD_SPEED_HS ? "HS" : "FS");

	return 0;
}

SYS_INIT(zlrs_usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
