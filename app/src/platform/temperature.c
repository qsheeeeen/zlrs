/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/temperature.h"

#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>

int zlrs_temperature_read(int16_t *centidegrees)
{
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(die_temp0)) && IS_ENABLED(CONFIG_SENSOR)
	const struct device *const sensor = DEVICE_DT_GET(DT_ALIAS(die_temp0));
	struct sensor_value value;
	if (!device_is_ready(sensor)) {
		return -ENODEV;
	}
	int rc = sensor_sample_fetch_chan(sensor, SENSOR_CHAN_DIE_TEMP);
	if (rc == 0) {
		rc = sensor_channel_get(sensor, SENSOR_CHAN_DIE_TEMP, &value);
	}
	if (rc == 0) {
		*centidegrees = (int16_t)(value.val1 * 100 + value.val2 / 10000);
	}
	return rc;
#else
	(void)centidegrees;
	return -ENOTSUP;
#endif
}
