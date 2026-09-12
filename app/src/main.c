/*
 * Copyright (c) 2026 zlrs contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

int main(void)
{
	LOG_INF("zlrs %s", APP_VERSION_STRING);

	/*
	 * TODO: 启动顺序按 docs/system.md 第 7 节：
	 *   1. 射频复位、SetStandby(XOSC)
	 *   2. Calibrate + CalibFE(LF 频点, HF 频点)
	 *   3. SetPacketType(FLRC) + SetFlrcModulationParams
	 *   4. SetRxPath + SetDioIrqConfig
	 *   5. 启动链路线程与串口线程
	 */

	return 0;
}
